/*
 * JTBassMIDI - LV2 plugin
 *
 * Monophonic bass-to-MIDI converter. It uses the same tracker as
 * JTBassSynth and sends MIDI notes instead of driving oscillators:
 *
 *   input -> HPF -> LPF -> decimate to ~6 kHz -> YIN period detector
 *                                                      |
 *   onset / gate detector --> note-on / note-off <-----+--> pitch bend
 *
 * The audio input passes through to the audio output (Thru level), so the
 * plugin can sit inline on a pedalboard.
 *
 * Copyright (c) 2026. ISC License.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#define JTBM_HAVE_SSE 1
#endif

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

#define JTBM_URI "urn:jtbassmidi:bass-midi"

#define JTBM_PI 3.14159265358979323846

typedef enum {
	P_IN = 0,
	P_OUT,
	P_MIDI_OUT,
	P_ENABLED,
	P_THRU,
	P_CHANNEL,
	P_TRANSPOSE,
	P_SENS,
	P_DYNAMICS,
	P_BEND,
	P_HOLD,
	P_NOTE,
	P_RELEASE,
	P_HOLD_MODE,
	P_COUNT
} PortIndex;

typedef struct {
	float b0, b1, b2, a1, a2;
	float z1, z2;
} Biquad;

#define YIN_BUF    1024    /* ring size in decimated samples (power of 2) */
#define YIN_RATE   6000.0  /* target analysis rate, Hz */
#define YIN_HOP    16      /* decimated samples between estimates (~2.7 ms) */
#define YIN_THRESH 0.12f   /* CMND dip threshold */

#define BEND_CENTER 8192
#define BEND_DELAY  4       /* readings a bend is held back (~11 ms) */
#define BEND_SETTLE 4       /* readings after a note start before bends follow */
#define CAND_N      6       /* agreeing readings needed for a change without a pluck */

typedef struct {
	void* port[P_COUNT];
	double fs;

	/* MIDI output */
	LV2_URID_Map*            map;
	LV2_URID                 midi_Event;
	LV2_Atom_Forge           forge;
	LV2_Atom_Forge_Frame     seq_frame;

	/* input conditioning */
	Biquad hp_in;
	Biquad aa1, aa2;    /* anti-alias low-pass ahead of decimation */

	/* pitch tracker: YIN on a ~6 kHz decimated copy of the input */
	int      dec_factor, dec_count, hop_count;
	double   fs_d;
	int      yin_w, yin_tmin, yin_tmax;
	float    ybuf[YIN_BUF];
	uint32_t ywr;       /* decimated samples written */
	uint32_t yavail;    /* decimated samples since the last onset */
	float    yx[YIN_BUF], yd[YIN_BUF];

	/* dynamics */
	float  env_fast, env_slow, env_amp;
	float  peak;        /* loudest env_amp since the onset, for velocity */
	bool   gate;
	double since_trig;

	/* damping detection: a ripple-free envelope (peak hold, then a fast
	 * release) compared with a slower reference that follows it */
	float  env_damp, env_ref;
	float  trough;      /* lowest env_damp since the damp, to spot the next pluck */
	int    damp_hold;   /* samples left before env_damp may fall */
	float  slow_max;    /* loudest env_slow since the onset */
	bool   attack_done; /* the note has started to decay */
	bool   decaying;    /* falling much faster than a ringing string */
	bool   damped;      /* string stopped: note ended until the next pluck */
	bool   off_pending; /* damped: note-off waits out a short grace period */
	int    damp_age;    /* samples since the damp */

	/* note logic (all pitches in fractional MIDI semitones) */
	bool   awaiting;    /* onset seen, waiting for three agreeing estimates */
	double start_est[3];
	float  start_dip[3];
	int    nstart;      /* consecutive clear estimates since the onset */
	float  dip_ref;     /* typical YIN dip for the sounding note */
	bool   have_prev;
	double prev;        /* previous raw estimate */
	bool   cand_active; /* a pitch change waiting for confirmation */
	double cand_est[CAND_N];
	float  cand_dip[CAND_N];
	int    ncand;
	double hist[3];     /* recent estimates, for a median bend value */
	int    hidx, hcount;
	double bq[BEND_DELAY]; /* bends held back, dropped if the string is damped */
	int    bq_n, bq_head;
	int    settle;      /* readings left before bends follow a new note */
	bool   has_base;
	int    base;        /* tracked note, before transpose */

	/* what the receiver currently has */
	int  cur_note;      /* sounding MIDI note, or -1 */
	int  cur_ch;
	int  cur_transpose;
	int  last_bend;     /* -1 forces the next bend to be sent */
	int  last_bend_ch;
	bool was_enabled;
	bool panic;         /* send All Notes Off on the next run */
	bool release_prev;  /* Release button state, for edge detection */

	/* per-run control values */
	int   ch, transpose, bend_range;
	bool  hold;
	bool  latch;        /* Hold Mode: false = Note Change, true = Latch */
	float thr_db, dyn;

	float thru_s;
} JTBassMIDI;

/* ---------------------------------------------------------------- helpers */

static inline float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

static inline int clampi(int x, int lo, int hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

static inline float coef_ms(double ms, double fs)
{
	if (ms <= 0.0) return 0.0f;
	return (float)exp(-1.0 / (ms * 0.001 * fs));
}

static void biquad_set(Biquad* b, double fs, double fc, double q, bool hp)
{
	const double w  = 2.0 * JTBM_PI * fc / fs;
	const double c  = cos(w);
	const double al = sin(w) / (2.0 * q);
	const double a0 = 1.0 + al;
	if (hp) {
		b->b0 = (float)((1.0 + c) * 0.5 / a0);
		b->b1 = (float)(-(1.0 + c) / a0);
	} else {
		b->b0 = (float)((1.0 - c) * 0.5 / a0);
		b->b1 = (float)((1.0 - c) / a0);
	}
	b->b2 = b->b0;
	b->a1 = (float)(-2.0 * c / a0);
	b->a2 = (float)((1.0 - al) / a0);
}

static inline float biquad_run(Biquad* b, float x)
{
	const float y = b->b0 * x + b->z1;
	b->z1 = b->b1 * x - b->a1 * y + b->z2;
	b->z2 = b->b2 * x - b->a2 * y;
	return y;
}

static inline double median3(double a, double b, double c)
{
	if (a > b) { double t = a; a = b; b = t; }
	if (b > c) { b = c; }
	return a > b ? a : b;
}

/* ------------------------------------------------------------- lifecycle */

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate,
                              const char* path, const LV2_Feature* const* f)
{
	(void)d; (void)path;
	LV2_URID_Map* map = NULL;
	for (int i = 0; f && f[i]; ++i)
		if (!strcmp(f[i]->URI, LV2_URID__map)) map = (LV2_URID_Map*)f[i]->data;
	if (!map) return NULL;

	JTBassMIDI* self = (JTBassMIDI*)calloc(1, sizeof(JTBassMIDI));
	if (!self) return NULL;
	self->fs  = rate;
	self->map = map;
	self->midi_Event = map->map(map->handle, LV2_MIDI__MidiEvent);
	lv2_atom_forge_init(&self->forge, map);

	biquad_set(&self->hp_in, rate, 28.0, 0.707, true);

	/* 4th-order Butterworth anti-alias filter, then decimate to ~6 kHz */
	biquad_set(&self->aa1, rate, 900.0, 0.5412, false);
	biquad_set(&self->aa2, rate, 900.0, 1.3066, false);
	self->dec_factor = (int)lrint(rate / YIN_RATE);
	if (self->dec_factor < 1) self->dec_factor = 1;
	self->fs_d     = rate / self->dec_factor;
	self->yin_w    = (int)lrint(0.015 * self->fs_d);     /* 15 ms window */
	self->yin_tmin = (int)floor(self->fs_d / 520.0);     /* highest note */
	self->yin_tmax = (int)ceil(self->fs_d / 28.0);       /* lowest note  */
	if (self->yin_tmin < 2) self->yin_tmin = 2;

	self->thru_s = 1.0f;
	return (LV2_Handle)self;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data)
{
	JTBassMIDI* self = (JTBassMIDI*)h;
	if (port < P_COUNT) self->port[port] = data;
}

static void reset_tracking(JTBassMIDI* self)
{
	self->gate = false;
	self->awaiting = false;
	self->have_prev = false;
	self->cand_active = false;
	self->hcount = 0;
	self->yavail = 0;
	self->hop_count = 0;
	self->attack_done = false;
	self->decaying = false;
	self->damped = false;
	self->off_pending = false; /* a pluck within the grace period keeps the note */
}

static void activate(LV2_Handle h)
{
	JTBassMIDI* self = (JTBassMIDI*)h;
	self->hp_in.z1 = self->hp_in.z2 = 0.0f;
	self->aa1.z1 = self->aa1.z2 = 0.0f;
	self->aa2.z1 = self->aa2.z2 = 0.0f;
	for (int i = 0; i < YIN_BUF; ++i) self->ybuf[i] = 0.0f;
	self->ywr = 0;
	self->dec_count = 0;
	self->env_fast = self->env_slow = self->env_amp = self->peak = 0.0f;
	self->env_damp = self->env_ref = self->slow_max = 0.0f;
	self->damp_hold = 0;
	reset_tracking(self);
	self->has_base = false;
	self->cur_note = -1;
	self->last_bend = -1;
	self->was_enabled = true;
	self->panic = true;
}

static void cleanup(LV2_Handle h)
{
	free(h);
}

/* ----------------------------------------------------------------- MIDI */

static void midi3(JTBassMIDI* self, uint32_t t, uint8_t a, uint8_t b, uint8_t c)
{
	const uint8_t msg[3] = { a, b, c };
	if (!lv2_atom_forge_frame_time(&self->forge, t)) return;
	if (!lv2_atom_forge_atom(&self->forge, 3, self->midi_Event)) return;
	lv2_atom_forge_write(&self->forge, msg, 3);
}

static void note_off(JTBassMIDI* self, uint32_t t)
{
	if (self->cur_note < 0) return;
	midi3(self, t, (uint8_t)(LV2_MIDI_MSG_NOTE_OFF | self->cur_ch),
	      (uint8_t)self->cur_note, 0);
	self->cur_note = -1;
}

/* Pitch bend, skipped when it moved less than ~2 cents (keeps DIN MIDI
 * from flooding). */
static void bend(JTBassMIDI* self, uint32_t t, double dev, bool force)
{
	int v = BEND_CENTER;
	int step = 1;
	if (self->bend_range > 0) {
		v = BEND_CENTER + (int)lrint(dev / self->bend_range * BEND_CENTER);
		step = (int)(2.0 * BEND_CENTER / (self->bend_range * 100.0));
		if (step < 1) step = 1;
	}
	v = clampi(v, 0, 16383);
	if (self->last_bend_ch != self->ch) self->last_bend = -1;
	if (self->last_bend >= 0 && !force && abs(v - self->last_bend) < step) return;
	if (v == self->last_bend) return;
	midi3(self, t, (uint8_t)(LV2_MIDI_MSG_BENDER | self->ch),
	      (uint8_t)(v & 0x7F), (uint8_t)((v >> 7) & 0x7F));
	self->last_bend    = v;
	self->last_bend_ch = self->ch;
}

/* Velocity from the attack level: Gate Threshold -> 1, -6 dBFS -> 127,
 * blended toward a fixed 100 by the Dynamics control. */
static uint8_t velocity(const JTBassMIDI* self, float amp)
{
	const float db = 20.0f * log10f(amp + 1e-9f);
	const float t  = clampf((db - self->thr_db) / (-6.0f - self->thr_db), 0.0f, 1.0f);
	const float v  = 100.0f + self->dyn * (1.0f + 126.0f * t - 100.0f);
	return (uint8_t)clampi((int)lrintf(v), 1, 127);
}

/* Replace whatever is sounding with the note nearest to `semi`. */
static void start_note(JTBassMIDI* self, uint32_t t, double semi, uint8_t vel)
{
	const int base = (int)lrint(semi);
	const int note = base + self->transpose;

	/* Hold: playing the sounding note again keeps sustaining it. */
	if (self->hold && note == self->cur_note && self->ch == self->cur_ch) {
		self->base = base;
		self->has_base = true;
		self->hcount = 0;
		self->bq_n = 0;
		self->settle = BEND_SETTLE;
		return;
	}

	note_off(self, t);
	self->base = base;
	self->has_base = true;
	self->hcount = 0;
	self->bq_n = 0;
	self->settle = BEND_SETTLE;
	if (note < 0 || note > 127) return;

	/* Start in tune: the first readings of a note run a little flat, so the
	 * bend starts centred and only follows once they have settled. */
	bend(self, t, 0.0, true);
	midi3(self, t, (uint8_t)(LV2_MIDI_MSG_NOTE_ON | self->ch), (uint8_t)note, vel);
	self->cur_note      = note;
	self->cur_ch        = self->ch;
	self->cur_transpose = self->transpose;
}

/*
 * Handle one pitch estimate (fractional MIDI note) at frame t.
 *
 * A new note needs three consecutive clear estimates that agree, so a
 * stray reading never reaches the synth. Without a new pluck, a pitch change
 * becomes a new note when it is abrupt (hammer-on, pull-off), or when it
 * drifts past the bend range (or past half a semitone with bends off), and
 * likewise needs three agreeing readings. Smaller, gradual changes become
 * pitch bend.
 *
 * `dip` is the YIN confidence (lower is cleaner). Once a note is sounding,
 * shaky readings (dip > 0.2) and readings from a string that is being
 * damped are ignored: that is where fret noise and the open string sneak in
 * when a finger lifts off.
 */
static void on_estimate(JTBassMIDI* self, double semi, float dip, uint32_t t)
{
	/* Latch mode: while Hold keeps a note, nothing the strings do changes
	 * it (no new notes, no bends) until Release. */
	if (self->hold && self->latch && self->cur_note >= 0) {
		self->awaiting = false;
		self->cand_active = false;
		return;
	}
	if (!self->awaiting && self->decaying) {
		self->bq_n = 0; /* the string is being damped: drop held-back bends */
		return;
	}
	if (self->awaiting) {
		/* Low notes still read a little flat for the first few ms, so only
		 * clear dips count, and three in a row must agree: within 0.12
		 * semitone (rock steady, even if the bass is out of tune), or within
		 * 0.3 and close to a semitone (not halfway between two notes). */
		self->prev = semi;
		self->have_prev = true;
		if (dip > YIN_THRESH) {
			self->nstart = 0;
			return;
		}
		self->start_est[self->nstart % 3] = semi;
		self->start_dip[self->nstart % 3] = dip;
		self->nstart++;
		if (self->nstart >= 3) {
			const double a = self->start_est[0], b = self->start_est[1], c = self->start_est[2];
			const double lo = fmin(a, fmin(b, c)), hi = fmax(a, fmax(b, c));
			const double avg = (a + b + c) / 3.0;
			const double off = fabs(avg - (double)lrint(avg));
			if (hi - lo < 0.12 || (hi - lo < 0.3 && off < 0.25)) {
				self->awaiting = false;
				self->cand_active = false;
				self->dip_ref = fmaxf(0.002f, fmaxf(self->start_dip[0],
				                      fmaxf(self->start_dip[1], self->start_dip[2])));
				start_note(self, t, avg, velocity(self, self->peak));
			}
		}
		return;
	}
	if (!self->has_base) {
		self->prev = semi;
		return;
	}

	/* Changes are measured against the sounding note, not the previous
	 * reading: after a one-off outlier, coming back to the sounding note is
	 * not a jump (that used to restart the same note). */
	const int    R      = self->bend_range;
	const double dev    = semi - self->base;
	const bool   moved  = lrint(semi) != self->base;
	const bool   jump   = moved && fabs(semi - self->prev) > 0.75;
	const bool   far    = moved && (R == 0 ? fabs(dev) > 0.6 : fabs(dev) > R + 0.5);
	/* Only clear readings update prev, so a jump is measured from the last
	 * clean reading of the old note to the first clean one of the new note,
	 * however many unclear readings came in between. */
	if (dip < YIN_THRESH) self->prev = semi;

	/* A change (hammer-on, pull-off, slide, or a pluck too soft to be
	 * detected as one) needs CAND_N clear readings in a row (~16 ms) that
	 * agree within 0.3 semitone and sit near a semitone. Readings taken
	 * while the pitch moves between two notes, or while the next finger
	 * touches the string before a damp, keep drifting and never pass.
	 * Coming back to the sounding note cancels it. */
	if (self->cand_active || jump || far) {
		if (dip >= YIN_THRESH) {
			self->cand_active = false;
			return;
		}
		if (moved) {
			if (!self->cand_active) self->ncand = 0;
			self->cand_active = true;
			self->cand_est[self->ncand % CAND_N] = semi;
			self->cand_dip[self->ncand % CAND_N] = dip;
			self->ncand++;
			if (self->ncand >= CAND_N) {
				double lo = 1e9, hi = -1e9, sum = 0.0;
				float  dmax = 0.002f;
				for (int k = 0; k < CAND_N; ++k) {
					lo = fmin(lo, self->cand_est[k]);
					hi = fmax(hi, self->cand_est[k]);
					sum += self->cand_est[k];
					dmax = fmaxf(dmax, self->cand_dip[k]);
				}
				const double avg = sum / CAND_N;
				if (hi - lo < 0.3 && fabs(avg - (double)lrint(avg)) < 0.3 &&
				    lrint(avg) != self->base) {
					self->cand_active = false;
					self->dip_ref = dmax;
					start_note(self, t, avg, velocity(self, self->env_amp));
				}
			}
			return;
		}
		self->cand_active = false;
	}

	/* Bends: each note learns its typical dip (cleanness), and readings
	 * much less clean than that are ignored. When the next finger touches
	 * the string, ~30 ms before the pluck and before the level drops, the
	 * dip jumps 10x and the pitch slides 0.3-0.8 semitone. A noisy pickup
	 * just has a higher typical dip. */
	if (dip > 0.2f || dip > 3.0f * self->dip_ref + 0.004f) return;
	self->dip_ref += 0.2f * (dip - self->dip_ref);
	self->hist[self->hidx] = semi;
	self->hidx = (self->hidx + 1) % 3;
	if (self->hcount < 3) self->hcount++;
	if (R > 0 && self->cur_note >= 0) {
		const double m = self->hcount >= 3
			? median3(self->hist[0], self->hist[1], self->hist[2]) : semi;
		if (self->settle > 0) {
			self->settle--;
			return;
		}
		/* Hold each bend back by BEND_DELAY readings. In the last ~10 ms
		 * before a damp the readings drift sharp; when the damp is spotted
		 * the queue is dropped, so that drift never reaches the synth. */
		if (self->bq_n == BEND_DELAY) {
			bend(self, t, self->bq[self->bq_head], false);
			self->bq_head = (self->bq_head + 1) % BEND_DELAY;
			self->bq_n--;
		}
		self->bq[(self->bq_head + self->bq_n) % BEND_DELAY] = m - self->base;
		self->bq_n++;
	}
}

/* ------------------------------------------------------------------ DSP */

/*
 * YIN period estimate over the most recent decimated samples (JTBassSynth's
 * tracker, with the reference window moved to the newest audio). Only
 * samples since the last onset are used, so the lag range grows as the note
 * rings: G-string notes resolve in ~28 ms, low E in ~44 ms.
 * Taking the first CMND dip under the threshold (not the deepest) avoids the
 * octave-low errors that the 2nd/3rd harmonics of a low string provoke.
 * Returns the period in decimated samples, or 0 if not confident yet.
 */
static double yin_estimate(JTBassMIDI* self, float* dip)
{
	const int W = self->yin_w;
	int tmax = self->yin_tmax;
	const int avail = (int)self->yavail - W - 2;
	if (avail < tmax) tmax = avail;
	if (tmax < self->yin_tmin + 2) return 0.0;

	/* Newest sample first: x[0..W) is the most recent audio and x[j + t] is
	 * t samples earlier. With oldest-first order the reference window stayed
	 * pinned to the attack transient until the lag range filled (~50 ms),
	 * which made the first readings of a note shaky and flat. */
	const int span = W + tmax + 2;
	float* x = self->yx;
	for (int j = 0; j < span; ++j)
		x[j] = self->ybuf[(self->ywr - 1u - (uint32_t)j) & (YIN_BUF - 1)];

	/* cumulative-mean-normalised difference function, lags 1..tmax+1 */
	float* d = self->yd;
	float run = 0.0f;
	d[0] = 1.0f;
	for (int t = 1; t <= tmax + 1; ++t) {
		float acc = 0.0f;
		for (int j = 0; j < W; ++j) {
			const float e = x[j] - x[j + t];
			acc += e * e;
		}
		run += acc;
		d[t] = run > 1e-20f ? acc * (float)t / run : 1.0f;
	}

	int best = 0;
	for (int t = self->yin_tmin; t <= tmax; ++t) {
		if (d[t] < YIN_THRESH) {
			while (t <= tmax && d[t + 1] < d[t]) ++t;
			best = t;
			break;
		}
	}
	if (!best) {
		/* no clear dip: accept the global minimum only if it is decent */
		float m = 0.3f;
		for (int t = self->yin_tmin; t <= tmax; ++t)
			if (d[t] < m && d[t] <= d[t - 1] && d[t] <= d[t + 1]) { m = d[t]; best = t; }
	}
	if (!best || best > tmax) return 0.0; /* still descending: wait for data */

	const float a = d[best - 1], b = d[best], c = d[best + 1];
	const float den = a - 2.0f * b + c;
	*dip = b;
	double off = den > 1e-9f ? 0.5 * (a - c) / den : 0.0;
	if (off > 0.5) off = 0.5;
	if (off < -0.5) off = -0.5;
	return best + off;
}

/* A new pluck: analyse only the new note. The previous note keeps sounding
 * until the new pitch is confirmed, so legato patches don't gap. */
static void trigger(JTBassMIDI* self)
{
	self->since_trig = 0.0;
	self->yavail = 0;
	self->hop_count = 0;
	self->awaiting = true;
	self->nstart = 0;
	self->have_prev = false;
	self->cand_active = false;
	self->peak = self->env_amp;
	self->slow_max = self->env_slow;
	self->attack_done = false;
	self->decaying = false;
	self->damped = false;
	self->off_pending = false; /* a pluck within the grace period keeps the note */
}

static void run(LV2_Handle h, uint32_t n)
{
	JTBassMIDI* self = (JTBassMIDI*)h;
	const double fs = self->fs;

#ifdef JTBM_HAVE_SSE
	const unsigned int csr = _mm_getcsr();
	_mm_setcsr(csr | 0x8040); /* FTZ | DAZ */
#elif defined(__aarch64__)
	uint64_t fpcr;
	__asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
	__asm__ volatile("msr fpcr, %0" : : "r"(fpcr | (1u << 24))); /* FZ */
#endif

	const float*       in  = (const float*)self->port[P_IN];
	float*             out = (float*)self->port[P_OUT];
	LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)self->port[P_MIDI_OUT];

	lv2_atom_forge_set_buffer(&self->forge, (uint8_t*)seq, seq->atom.size);
	lv2_atom_forge_sequence_head(&self->forge, &self->seq_frame, 0);

	const bool  enabled = *(float*)self->port[P_ENABLED] > 0.5f;
	const float thru    = clampf(*(float*)self->port[P_THRU], 0.0f, 1.0f);
	const bool  hold    = *(float*)self->port[P_HOLD] > 0.5f;
	self->hold       = hold;
	self->latch      = *(float*)self->port[P_HOLD_MODE] > 0.5f;
	self->ch         = clampi((int)lrintf(*(float*)self->port[P_CHANNEL]), 1, 16) - 1;
	self->transpose  = clampi((int)lrintf(*(float*)self->port[P_TRANSPOSE]), -36, 36);
	self->bend_range = clampi((int)lrintf(*(float*)self->port[P_BEND]), 0, 24);
	self->thr_db     = clampf(*(float*)self->port[P_SENS], -80.0f, -10.0f);
	self->dyn        = clampf(*(float*)self->port[P_DYNAMICS], 0.0f, 1.0f);

	if (self->panic) {
		self->panic = false;
		midi3(self, 0, (uint8_t)(LV2_MIDI_MSG_CONTROLLER | self->ch), LV2_MIDI_CTL_ALL_NOTES_OFF, 0);
		bend(self, 0, 0.0, true);
	}

	/* Bypassed (MOD footswitch): release the note, pass audio through. */
	if (!enabled) {
		if (self->was_enabled) {
			note_off(self, 0);
			bend(self, 0, 0.0, true);
			reset_tracking(self);
			self->has_base = false;
			self->was_enabled = false;
		}
		if (out != in) memcpy(out, in, n * sizeof(float));
		lv2_atom_forge_pop(&self->forge, &self->seq_frame);
		*(float*)self->port[P_NOTE] = -1.0f;
		goto done;
	}
	self->was_enabled = true;

	/* Channel or transpose changed under a held note: release it, and
	 * replay at the new mapping once the pitch is re-confirmed. */
	if (self->cur_note >= 0 &&
	    (self->ch != self->cur_ch || self->transpose != self->cur_transpose)) {
		note_off(self, 0);
		if (self->gate) {
			self->awaiting  = true;
			self->nstart    = 0;
			self->have_prev = false;
		}
	}
	/* Release button (momentary): end the held note on the press. The string
	 * may still be ringing, so ignore it until the next pluck. */
	const bool release = *(float*)self->port[P_RELEASE] > 0.5f;
	if (release && !self->release_prev) {
		note_off(self, 0);
		self->has_base    = false;
		self->awaiting    = false;
		self->cand_active = false;
	}
	self->release_prev = release;

	/* Hold switched off while a note was being held after the gate closed. */
	if (!hold && !self->gate) note_off(self, 0);
	if (self->bend_range == 0 && self->last_bend >= 0 && self->last_bend != BEND_CENTER)
		bend(self, 0, 0.0, true);

	/* envelope coefficients (same as JTBassSynth) */
	const float fast_a = coef_ms(0.3, fs),  fast_r = coef_ms(12.0, fs);
	const float slow_a = coef_ms(25.0, fs), slow_r = coef_ms(250.0, fs);
	const float amp_a  = coef_ms(2.0, fs),  amp_r  = coef_ms(70.0, fs);
	const float smooth = 1.0f - coef_ms(20.0, fs);
	const float thr_on  = powf(10.0f, self->thr_db / 20.0f);
	const float thr_off = thr_on * 0.5f;
	const double holdoff = 0.07 * fs;
	const int   damp_hold_n = (int)(0.012 * fs);
	const float damp_r = coef_ms(12.0, fs);
	const float ref_a  = coef_ms(10.0, fs), ref_r = coef_ms(80.0, fs);
	const double damp_min_age = 0.1 * fs;
	const int   damp_grace_n = (int)(0.04 * fs);

	for (uint32_t i = 0; i < n; ++i) {
		const float x  = in[i];
		const float xh = biquad_run(&self->hp_in, x);
		const float ax = fabsf(xh);

		/* --- envelope followers ------------------------------------- */
		self->env_fast = ax + (self->env_fast - ax) * (ax > self->env_fast ? fast_a : fast_r);
		self->env_slow = ax + (self->env_slow - ax) * (ax > self->env_slow ? slow_a : slow_r);
		self->env_amp  = ax + (self->env_amp  - ax) * (ax > self->env_amp  ? amp_a  : amp_r);
		if (self->awaiting && self->env_amp > self->peak) self->peak = self->env_amp;

		/* Damping envelope: hold each peak for 12 ms (longer than half a
		 * cycle of low B, so it doesn't ripple), then fall with 12 ms. */
		if (ax >= self->env_damp) {
			self->env_damp  = ax;
			self->damp_hold = damp_hold_n;
		} else if (self->damp_hold > 0) {
			self->damp_hold--;
		} else {
			self->env_damp *= damp_r;
		}
		self->env_ref = self->env_damp + (self->env_ref - self->env_damp) *
		                (self->env_damp > self->env_ref ? ref_a : ref_r);

		/* Attack phase ends once the slow envelope has dropped 1 dB below
		 * its peak. A light, slowly building pluck stays in its attack. */
		if (self->env_slow > self->slow_max) self->slow_max = self->env_slow;
		else if (self->env_slow < 0.89f * self->slow_max) self->attack_done = true;

		/* A ringing string decays at a few tens of dB per second, which
		 * keeps env_damp within ~3-7 dB of env_ref. Lifting a finger off a
		 * fret drops it far faster. Below -6 dB, pitch readings are ignored.
		 * (They only steer the pitch, so a fast natural decay that trips this
		 * just holds the pitch it already has.) */
		if (self->damped && self->env_damp < self->trough) self->trough = self->env_damp;
		self->decaying = self->gate && self->attack_done &&
		                 self->env_damp < 0.5f * self->env_ref;

		/* --- gate & note-onset detection ----------------------------- */
		self->since_trig += 1.0;
		if (!self->gate && self->env_amp > thr_on) {
			self->gate = true;
			trigger(self);
		} else if (self->gate && self->env_amp < thr_off) {
			self->gate = false;
			self->awaiting = false;
			if (!hold) note_off(self, i);
		} else if (self->gate && self->damped &&
		           self->env_damp > 4.0f * self->trough && self->env_fast > 2.0f * thr_on) {
			/* Next pluck after a damp: a 12 dB rise from the bottom of the
			 * dip. Fingerstyle players damp each note with the next finger
			 * for 15-25 ms just before plucking, too briefly for env_slow
			 * to fall, so the rule below would miss it. */
			trigger(self);
		} else if (self->gate && self->attack_done && self->since_trig > holdoff &&
		           self->env_fast > 2.0f * self->env_slow &&
		           self->env_fast > 2.0f * thr_on) {
			/* New pluck while still ringing. Requiring the note to have
			 * started decaying stops a soft attack that is still building
			 * from counting as a second pluck. */
			trigger(self);
		} else if (self->gate && !self->damped && self->attack_done &&
		           self->since_trig > damp_min_age &&
		           self->env_damp < 0.2f * self->env_ref) {
			/* String damped (finger lifted, or the next finger landing):
			 * stop tracking its noisy tail, and end the note after a 40 ms
			 * grace period unless another pluck comes first. Then the old
			 * note runs straight into the new one, as on a re-pluck. */
			self->damped = true;
			self->trough = self->env_damp;
			self->awaiting = false;
			self->cand_active = false;
			self->off_pending = true;
			self->damp_age = 0;
			self->bq_n = 0;
		}
		if (self->off_pending && ++self->damp_age > damp_grace_n) {
			self->off_pending = false;
			if (!hold) note_off(self, i);
		}

		/* --- pitch tracker ------------------------------------------ */
		const float xa = biquad_run(&self->aa2, biquad_run(&self->aa1, xh));
		if (++self->dec_count >= self->dec_factor) {
			self->dec_count = 0;
			self->ybuf[self->ywr & (YIN_BUF - 1)] = xa;
			self->ywr++;
			if (self->gate) {
				if (self->yavail < YIN_BUF - 1) self->yavail++;
				if (++self->hop_count >= YIN_HOP) {
					self->hop_count = 0;
					float dip = 1.0f;
					const double p = yin_estimate(self, &dip);
					if (p > 0.0 && !self->damped)
						on_estimate(self, 69.0 + 12.0 * log2(self->fs_d / p / 440.0), dip, i);
				}
			}
		}

		/* --- audio thru --------------------------------------------- */
		self->thru_s += (thru - self->thru_s) * smooth;
		out[i] = self->thru_s * x;
	}

	lv2_atom_forge_pop(&self->forge, &self->seq_frame);
	*(float*)self->port[P_NOTE] = (float)self->cur_note;

done:
#ifdef JTBM_HAVE_SSE
	_mm_setcsr(csr);
#elif defined(__aarch64__)
	__asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
	return;
}

static const void* extension_data(const char* uri)
{
	(void)uri;
	return NULL;
}

static const LV2_Descriptor descriptor = {
	JTBM_URI,
	instantiate,
	connect_port,
	activate,
	run,
	NULL, /* deactivate */
	cleanup,
	extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
	return index == 0 ? &descriptor : NULL;
}
