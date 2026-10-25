/*
 * Offline test for JTBassMIDI: synthetic bass plucks in, MIDI out.
 *
 * Builds the plugin source directly (no host needed), runs it in 128-frame
 * blocks at 48 kHz, and reports note accuracy and note-on latency (time
 * from the pluck to the note-on event) per string, plus legato re-plucks,
 * hammer-ons, bends and velocity.
 *
 *   make test
 */

#include "../JTBassMIDI.c"

#include <lv2/atom/util.h>
#include <stdio.h>

#define FS     48000
#define BLOCK  128
#define MAXLEN (FS * 8)

/* ------------------------------------------------------------ URID map */

static const char* uris[64];
static int nuris;

static LV2_URID map_uri(LV2_URID_Map_Handle h, const char* uri)
{
	(void)h;
	for (int i = 0; i < nuris; ++i)
		if (!strcmp(uris[i], uri)) return (LV2_URID)(i + 1);
	uris[nuris++] = uri;
	return (LV2_URID)nuris;
}

/* ------------------------------------------------------------- harness */

typedef struct {
	long    t;          /* absolute frame */
	uint8_t m[3];
} Ev;

static Ev  evs[65536];
static int nev;

/* Frame at which the Release button is pressed for one block, or -1. */
static long release_at = -1;

typedef struct {
	float thru, channel, transpose, sens, dyn, bend, hold, enabled, note, hold_mode;
} Controls;

static void run_plugin(const float* in, long len, Controls c)
{
	LV2_URID_Map map = { NULL, map_uri };
	LV2_Feature  fmap = { LV2_URID__map, &map };
	const LV2_Feature* feats[] = { &fmap, NULL };

	LV2_Handle h = descriptor.instantiate(&descriptor, FS, "", feats);
	static float out[BLOCK];
	static uint64_t atom_buf[2048];
	LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)atom_buf;

	float* ctl[] = { &c.enabled, &c.thru, &c.channel, &c.transpose,
	                 &c.sens, &c.dyn, &c.bend, &c.hold };
	descriptor.connect_port(h, P_MIDI_OUT, seq);
	descriptor.connect_port(h, P_OUT, out);
	for (int i = 0; i < 8; ++i) descriptor.connect_port(h, P_ENABLED + i, ctl[i]);
	descriptor.connect_port(h, P_NOTE, &c.note);
	float release = 0.0f;
	descriptor.connect_port(h, P_RELEASE, &release);
	descriptor.connect_port(h, P_HOLD_MODE, &c.hold_mode);
	descriptor.activate(h);

	nev = 0;
	for (long pos = 0; pos < len; pos += BLOCK) {
		const uint32_t n = (uint32_t)(len - pos < BLOCK ? len - pos : BLOCK);
		descriptor.connect_port(h, P_IN, (void*)(in + pos));
		release = (release_at >= pos && release_at < pos + BLOCK) ? 1.0f : 0.0f;
		seq->atom.size = sizeof(atom_buf) - sizeof(LV2_Atom);
		descriptor.run(h, n);
		LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
			const uint8_t* m = (const uint8_t*)LV2_ATOM_BODY(&ev->body);
			Ev* e = &evs[nev++];
			e->t = pos + ev->time.frames;
			memcpy(e->m, m, 3);
		}
	}
	descriptor.cleanup(h);
}

static Controls defaults(void)
{
	Controls c = { 1.0f, 1.0f, 0.0f, -48.0f, 0.7f, 2.0f, 0.0f, 1.0f, 0.0f, 0.0f };
	return c;
}

/* --------------------------------------------------------- pluck model */

typedef struct { const char* name; float h[8]; } Mix;

/* Partial amplitudes 1..8. "weak fund" mimics a bridge pickup or a small
 * speaker where the 2nd harmonic dominates - the classic octave trap. */
static const Mix MIXES[] = {
	{ "round",     { 1.00f, 0.50f, 0.30f, 0.15f, 0.08f, 0.05f, 0.03f, 0.02f } },
	{ "bright",    { 1.00f, 0.80f, 0.70f, 0.50f, 0.40f, 0.30f, 0.20f, 0.15f } },
	{ "weak fund", { 0.35f, 1.00f, 0.80f, 0.40f, 0.25f, 0.15f, 0.08f, 0.05f } },
};
#define NMIX (int)(sizeof(MIXES) / sizeof(MIXES[0]))

static uint32_t rng = 1;
static float noise(void)
{
	rng = rng * 1664525u + 1013904223u;
	return (float)((rng >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}

static double midi_hz(double note) { return 440.0 * pow(2.0, (note - 69.0) / 12.0); }

/*
 * Add a plucked note: stiff-string partials with per-partial decay, a short
 * noise click, and a 15 ms damp at the end. `glide_to`/`glide_at`/`glide_ms`
 * change the pitch mid-note without a new attack (hammer-on or bend).
 */
static void pluck(float* buf, long start, long dur, double note, float amp,
                  const Mix* mix, double glide_to, long glide_at, double glide_ms)
{
	double ph[8] = { 0 };
	const double B = 0.00012;   /* string inharmonicity */
	const long damp = FS * 15 / 1000;
	for (long i = 0; i < dur; ++i) {
		const double t = (double)i / FS;
		double nn = note;
		if (glide_at > 0 && i >= glide_at) {
			const double g = glide_ms > 0.0 ? (i - glide_at) / (glide_ms * 0.001 * FS) : 1.0;
			nn = note + (glide_to - note) * (g > 1.0 ? 1.0 : g);
		}
		const double f = midi_hz(nn);
		double s = 0.0;
		for (int k = 0; k < 8; ++k) {
			const double fk = (k + 1) * f * sqrt(1.0 + B * (k + 1) * (k + 1));
			if (fk > 0.45 * FS) continue;
			ph[k] += fk / FS;
			ph[k] -= floor(ph[k]);
			s += mix->h[k] * exp(-t * (1.2 + 0.9 * k)) * sin(2.0 * JTBM_PI * ph[k]);
		}
		float a = amp * (float)(1.0 - exp(-t / 0.0008));
		if (i > dur - damp) a *= (float)(dur - i) / damp;
		float click = i < FS / 300 ? 0.08f * amp * noise() : 0.0f;
		buf[start + i] += a * (float)s * 0.45f + click;
	}
}

/*
 * A fretted note played the way that showed artifacts on real hardware:
 * a light pluck that swells in over `attack_ms` (raised cosine) and, after
 * `dur` samples, a finger lifted off the fret. On the lift the fretted note
 * dies with a 15 ms time constant, a short fret click sounds, and the open
 * string (`open_note`) rings on quietly underneath (-26 dB, 150 ms decay).
 */
static void pluck_lift(float* buf, long start, long dur, double note, double open_note,
                       float amp, double attack_ms)
{
	double ph[8] = { 0 }, po[4] = { 0 };
	const double B = 0.00012;
	const long att = (long)(attack_ms * 0.001 * FS);
	const long tail = FS / 3;
	for (long i = 0; i < dur + tail; ++i) {
		const double t = (double)i / FS;
		const double f = midi_hz(note);
		double s = 0.0;
		for (int k = 0; k < 8; ++k) {
			const double fk = (k + 1) * f * sqrt(1.0 + B * (k + 1) * (k + 1));
			ph[k] += fk / FS;
			ph[k] -= floor(ph[k]);
			s += MIXES[0].h[k] * exp(-t * (1.2 + 0.9 * k)) * sin(2.0 * JTBM_PI * ph[k]);
		}
		double a = i < att ? 0.5 - 0.5 * cos(JTBM_PI * i / att) : 1.0;
		double y = 0.0;
		if (i >= dur) {
			const double tl = (double)(i - dur) / FS;
			a *= exp(-tl / 0.015);
			const double fo = midi_hz(open_note);
			double so = 0.0;
			for (int k = 0; k < 4; ++k) {
				po[k] += (k + 1) * fo / FS;
				po[k] -= floor(po[k]);
				so += MIXES[0].h[k] * sin(2.0 * JTBM_PI * po[k]);
			}
			y += 0.05 * amp * (1.0 - exp(-tl / 0.003)) * exp(-tl / 0.15) * so * 0.45;
			if (i - dur < FS / 200) y += 0.15 * amp * noise() * exp(-tl / 0.0015);
		}
		buf[start + i] += (float)(amp * a * s * 0.45 + y);
	}
}

/* --------------------------------------------------------------- checks */

static int failures;

static void check(bool ok, const char* what)
{
	printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
	if (!ok) failures++;
}

static bool is_on(const Ev* e)   { return (e->m[0] & 0xF0) == 0x90 && e->m[2] > 0; }
static bool is_off(const Ev* e)  { return (e->m[0] & 0xF0) == 0x80 || ((e->m[0] & 0xF0) == 0x90 && e->m[2] == 0); }
static bool is_bend(const Ev* e) { return (e->m[0] & 0xF0) == 0xE0; }

static float buf[MAXLEN];

static void test_strings(void)
{
	static const struct { const char* name; int open; int frets; } strings[] = {
		{ "B (5-str)", 23, 12 }, { "E", 28, 12 }, { "A", 33, 12 },
		{ "D", 38, 12 }, { "G", 43, 24 },
	};
	static const int frets[] = { 0, 1, 3, 5, 7, 9, 12, 15, 17, 19, 21, 24 };

	printf("\nSingle notes: note-on latency from the pluck (ms)\n");
	printf("  %-10s %-10s %6s %6s %6s %s\n", "string", "tone", "min", "avg", "max", "wrong/missed");

	for (size_t s = 0; s < sizeof(strings) / sizeof(strings[0]); ++s) {
		for (int m = 0; m < NMIX; ++m) {
			double lsum = 0.0, lmin = 1e9, lmax = 0.0;
			int count = 0, bad = 0;
			for (size_t fi = 0; fi < sizeof(frets) / sizeof(frets[0]); ++fi) {
				if (frets[fi] > strings[s].frets) continue;
				const int note = strings[s].open + frets[fi];
				const long onset = FS / 5, len = FS;
				memset(buf, 0, sizeof(float) * len);
				pluck(buf, onset, FS / 2, note, 0.3f, &MIXES[m], 0, 0, 0);
				run_plugin(buf, len, defaults());

				int ons = 0; bool right = false, offed = false;
				for (int e = 0; e < nev; ++e) {
					if (is_on(&evs[e])) {
						ons++;
						if (ons == 1 && evs[e].m[1] == note) {
							right = true;
							const double ms = (evs[e].t - onset) * 1000.0 / FS;
							lsum += ms; count++;
							if (ms < lmin) lmin = ms;
							if (ms > lmax) lmax = ms;
						}
					} else if (is_off(&evs[e])) {
						offed = true;
					}
				}
				if (!right || ons != 1 || !offed) {
					bad++;
					printf("    ! %s fret %d (note %d, %s): %d note-ons, first %s, note-off %s\n",
					       strings[s].name, frets[fi], note, MIXES[m].name, ons,
					       right ? "right" : "wrong/missing", offed ? "yes" : "no");
				}
			}
			printf("  %-10s %-10s %6.1f %6.1f %6.1f %d\n", strings[s].name, MIXES[m].name,
			       count ? lmin : 0.0, count ? lsum / count : 0.0, count ? lmax : 0.0, bad);
			if (bad) failures++;
		}
	}
}

static void test_legato(void)
{
	printf("\nRe-pluck while the previous note rings (E1 -> A1 -> D2)\n");
	const long len = FS * 2;
	memset(buf, 0, sizeof(float) * len);
	pluck(buf, FS / 5, FS * 2 / 5 + FS / 50, 28, 0.3f, &MIXES[0], 0, 0, 0);
	pluck(buf, FS * 3 / 5, FS * 2 / 5 + FS / 50, 33, 0.3f, &MIXES[0], 0, 0, 0);
	pluck(buf, FS, FS / 2, 38, 0.3f, &MIXES[0], 0, 0, 0);
	run_plugin(buf, len, defaults());
	int seen[3] = { 0 }, n = 0, wrong = 0;
	for (int e = 0; e < nev; ++e)
		if (is_on(&evs[e])) {
			const int k = evs[e].m[1] == 28 ? 0 : evs[e].m[1] == 33 ? 1 : evs[e].m[1] == 38 ? 2 : -1;
			if (k < 0) wrong++; else seen[k]++;
			n++;
		}
	printf("  note-ons: %d (E1 %d, A1 %d, D2 %d, other %d)\n", n, seen[0], seen[1], seen[2], wrong);
	check(seen[0] == 1 && seen[1] == 1 && seen[2] == 1 && wrong == 0, "one note-on per pluck, no extras");
}

static void test_hammer_on(void)
{
	printf("\nHammer-on without a new attack (A1 -> C2 at 300 ms)\n");
	const long len = FS + FS / 2;
	memset(buf, 0, sizeof(float) * len);
	pluck(buf, FS / 5, FS, 33, 0.3f, &MIXES[0], 36, FS * 3 / 10, 0.0);
	run_plugin(buf, len, defaults());
	int on33 = 0, on36 = 0, other = 0;
	long t36 = -1;
	for (int e = 0; e < nev; ++e)
		if (is_on(&evs[e])) {
			if (evs[e].m[1] == 33) on33++;
			else if (evs[e].m[1] == 36) { on36++; if (t36 < 0) t36 = evs[e].t; }
			else other++;
		}
	if (t36 >= 0)
		printf("  C2 note-on %.1f ms after the hammer-on\n", (t36 - (FS / 5 + FS * 3 / 10)) * 1000.0 / FS);
	check(on33 == 1 && on36 == 1 && other == 0, "A1 then C2, no stray notes");
}

static void test_bend(void)
{
	printf("\nSlow bend +1 semitone over 200 ms on D2, bend range 2\n");
	const long len = FS + FS / 2;
	memset(buf, 0, sizeof(float) * len);
	pluck(buf, FS / 5, FS, 38, 0.3f, &MIXES[0], 39, FS * 3 / 10, 200.0);
	run_plugin(buf, len, defaults());
	int ons = 0, bends = 0, maxv = 0;
	for (int e = 0; e < nev; ++e) {
		if (is_on(&evs[e])) ons++;
		if (is_bend(&evs[e])) {
			const int v = evs[e].m[1] | (evs[e].m[2] << 7);
			bends++;
			if (v > maxv) maxv = v;
		}
	}
	const double semis = (maxv - 8192) / 8192.0 * 2.0;
	printf("  %d bend messages, peak %+.2f semitones\n", bends, semis);
	check(ons == 1, "one note-on (the bend did not retrigger)");
	check(fabs(semis - 1.0) < 0.1, "bend peaks at +1 semitone (within 10 cents)");

	printf("\nSame bend with Bend Range = Off\n");
	Controls c = defaults();
	c.bend = 0.0f;
	run_plugin(buf, len, c);
	int on38 = 0, on39 = 0, other = 0;
	bends = 0;
	for (int e = 0; e < nev; ++e) {
		if (is_on(&evs[e])) {
			if (evs[e].m[1] == 38) on38++;
			else if (evs[e].m[1] == 39) on39++;
			else other++;
		}
		if (is_bend(&evs[e]) && (evs[e].m[1] | (evs[e].m[2] << 7)) != 8192) bends++;
	}
	check(on38 == 1 && on39 == 1 && other == 0 && bends == 0,
	      "quantised: D2 then D#2, no pitch bend");
}

static void test_velocity(void)
{
	printf("\nVelocity follows picking strength (A1)\n");
	const float amps[] = { 0.03f, 0.1f, 0.3f, 0.8f };
	int prev = 0;
	bool rising = true;
	for (int i = 0; i < 4; ++i) {
		memset(buf, 0, sizeof(buf));
		pluck(buf, FS / 5, FS / 2, 33, amps[i], &MIXES[0], 0, 0, 0);
		run_plugin(buf, FS, defaults());
		int vel = 0;
		for (int e = 0; e < nev; ++e)
			if (is_on(&evs[e])) { vel = evs[e].m[2]; break; }
		printf("  amplitude %.2f -> velocity %d\n", amps[i], vel);
		if (vel <= prev) rising = false;
		prev = vel;
	}
	check(rising, "velocity rises with amplitude");
}

static void test_hold_and_transpose(void)
{
	printf("\nHold on, transpose +12 (E1)\n");
	Controls c = defaults();
	c.hold = 1.0f;
	c.transpose = 12.0f;
	memset(buf, 0, sizeof(buf));
	pluck(buf, FS / 5, FS / 2, 28, 0.3f, &MIXES[0], 0, 0, 0);
	run_plugin(buf, FS, c);
	int on40 = 0, offs = 0;
	for (int e = 0; e < nev; ++e) {
		if (is_on(&evs[e]) && evs[e].m[1] == 40) on40++;
		if (is_off(&evs[e])) offs++;
	}
	check(on40 == 1, "note-on is E2 (transposed up an octave)");
	check(offs == 0, "note held after the string is damped");
}

static void test_release(void)
{
	printf("\nHold on: Release button ends the held note (A1, Release at 1.0 s)\n");
	Controls c = defaults();
	c.hold = 1.0f;
	const long len = FS * 3 / 2;
	memset(buf, 0, sizeof(float) * len);
	pluck(buf, FS / 5, FS / 2, 33, 0.3f, &MIXES[0], 0, 0, 0);
	release_at = FS;
	run_plugin(buf, len, c);
	int ons = 0, offs = 0;
	long toff = -1;
	for (int e = 0; e < nev; ++e) {
		if (is_on(&evs[e])) ons++;
		if (is_off(&evs[e])) { offs++; toff = evs[e].t; }
	}
	check(ons == 1 && offs == 1 && toff >= FS && toff < FS + BLOCK,
	      "one note-on, note-off on the Release press");

	printf("\nRelease pressed while the string still rings (A1, Release at 400 ms)\n");
	release_at = FS * 2 / 5;
	run_plugin(buf, len, c);
	ons = offs = 0;
	for (int e = 0; e < nev; ++e) {
		if (is_on(&evs[e])) ons++;
		if (is_off(&evs[e])) offs++;
	}
	check(ons == 1 && offs == 1, "note ends and the ringing string does not restart it");

	printf("\nNext note after Release still plays (A1, Release, D2)\n");
	memset(buf, 0, sizeof(float) * len);
	pluck(buf, FS / 5, FS * 2 / 5, 33, 0.3f, &MIXES[0], 0, 0, 0);
	pluck(buf, FS, FS * 2 / 5, 38, 0.3f, &MIXES[0], 0, 0, 0);
	release_at = FS * 7 / 10;
	run_plugin(buf, len, c);
	int on38 = 0;
	for (int e = 0; e < nev; ++e)
		if (is_on(&evs[e]) && evs[e].m[1] == 38) on38++;
	check(on38 == 1, "D2 note-on after the release");
	release_at = -1;

	printf("\nHold on: same note plucked again keeps sustaining (A1, A1, then D2)\n");
	memset(buf, 0, sizeof(float) * len);
	pluck(buf, FS / 5, FS / 4, 33, 0.3f, &MIXES[0], 0, 0, 0);      /* damped, then held */
	pluck(buf, FS * 3 / 5, FS / 4, 33, 0.3f, &MIXES[0], 0, 0, 0);  /* same note again */
	pluck(buf, FS, FS / 4, 38, 0.3f, &MIXES[0], 0, 0, 0);          /* new note */
	run_plugin(buf, len, c);
	int on33 = 0, offs33 = 0;
	on38 = 0;
	for (int e = 0; e < nev; ++e) {
		if (is_on(&evs[e]) && evs[e].m[1] == 33) on33++;
		if (is_on(&evs[e]) && evs[e].m[1] == 38) on38++;
		if (is_off(&evs[e]) && evs[e].m[1] == 33) offs33++;
	}
	check(on33 == 1, "A1 sent once, not restarted by the second pluck");
	check(on38 == 1 && offs33 == 1, "D2 replaces it with one note-off and one note-on");
}

static void test_latch(void)
{
	printf("\nHold Mode = Latch: other notes ignored until Release (A1, D2, Release, G2)\n");
	Controls c = defaults();
	c.hold = 1.0f;
	c.hold_mode = 1.0f; /* Latch */
	const long len = FS * 2;
	memset(buf, 0, sizeof(float) * len);
	pluck(buf, FS / 5, FS / 4, 33, 0.3f, &MIXES[0], 0, 0, 0);
	pluck(buf, FS * 3 / 5, FS / 4, 38, 0.3f, &MIXES[0], 0, 0, 0);
	pluck(buf, FS * 3 / 2, FS / 4, 43, 0.3f, &MIXES[0], 0, 0, 0);
	release_at = FS * 6 / 5;
	run_plugin(buf, len, c);
	release_at = -1;
	int on33 = 0, on38 = 0, on43 = 0, other = 0, offs = 0, bends = 0;
	long toff = -1;
	for (int e = 0; e < nev; ++e) {
		if (is_on(&evs[e])) {
			if (evs[e].m[1] == 33) on33++;
			else if (evs[e].m[1] == 38) on38++;
			else if (evs[e].m[1] == 43) on43++;
			else other++;
		}
		if (is_off(&evs[e]) && toff < 0) { offs++; toff = evs[e].t; }
		if (is_bend(&evs[e]) && evs[e].t > FS * 3 / 5 && evs[e].t < FS * 6 / 5) bends++;
	}
	check(on33 == 1 && on38 == 0, "A1 latched; D2 sends nothing");
	check(bends == 0, "no pitch bend from D2 while latched");
	check(toff >= FS * 6 / 5 && toff < FS * 6 / 5 + BLOCK, "first note-off is the Release press");
	check(on43 == 1 && other == 0, "G2 plays after the release");
}
/* Light pluck that swells in, then a finger lifted off the fret. Expect one
 * note-on, a note-off soon after the lift, no open-string note and no pitch
 * bend from the noisy tail. Run with bends on and off. */
static void test_light_pluck_and_lift(void)
{
	static const struct { int note, open; const char* name; } cases[] = {
		{ 31, 28, "G1 (E string, fret 3)" }, { 36, 33, "C2 (A string, fret 3)" },
		{ 40, 38, "E2 (D string, fret 2)" }, { 47, 43, "B2 (G string, fret 4)" },
	};
	printf("\nLight pluck (40 ms swell, -26 dB) then finger lifted at 600 ms\n");
	for (int bend_on = 1; bend_on >= 0; --bend_on) {
		for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
			Controls c = defaults();
			c.bend = bend_on ? 2.0f : 0.0f;
			const long onset = FS / 5, lift = onset + FS * 3 / 5, len = FS * 3 / 2;
			memset(buf, 0, sizeof(float) * len);
			pluck_lift(buf, onset, lift - onset, cases[k].note, cases[k].open, 0.05f, 40.0);
			run_plugin(buf, len, c);
			int ons = 0, right = 0, late_bend = 0;
			long toff = -1;
			for (int e = 0; e < nev; ++e) {
				if (is_on(&evs[e])) { ons++; if (evs[e].m[1] == cases[k].note) right++; }
				if (is_off(&evs[e]) && toff < 0) toff = evs[e].t;
				if (is_bend(&evs[e]) && evs[e].t > lift) {
					const int v = (evs[e].m[1] | (evs[e].m[2] << 7)) - 8192;
					if (abs(v) > 410) late_bend++; /* more than 10 cents at range 2 */
				}
			}
			const double off_ms = toff < 0 ? -1.0 : (toff - lift) * 1000.0 / FS;
			char what[160];
			snprintf(what, sizeof what, "%s, bend %s: %d note-on, note-off %.0f ms after lift, %d bends after lift",
			         cases[k].name, bend_on ? "on " : "off", ons, off_ms, late_bend);
			check(ons == 1 && right == 1 && off_ms >= 0.0 && off_ms < 80.0 && late_bend == 0, what);
		}
	}
}

/* The scale from a real recording that exposed a bug: G major up and down,
 * fingerstyle, each note damped by the next finger for ~20 ms just before
 * it is plucked. Expect exactly one note-on per note, each new note
 * replacing the last with no gap, and no stray pitch bend. */
static const int SCALE[] = { 31, 33, 35, 36, 38, 40, 42, 43, 42, 40, 38, 36, 35, 33, 31 };
#define NSCALE (int)(sizeof(SCALE) / sizeof(SCALE[0]))

static long scale_buf(float amp)
{
	const long step = FS * 38 / 100, dur = FS * 36 / 100; /* 15 ms damp, ~5 ms gap */
	const long len = FS / 5 + step * NSCALE + FS / 2;
	memset(buf, 0, sizeof(buf));
	for (int k = 0; k < NSCALE; ++k)
		pluck(buf, FS / 5 + k * step, dur, SCALE[k], amp, &MIXES[0], 0, 0, 0);
	return len;
}

static void test_fingerstyle_scale(void)
{
	printf("\nFingerstyle scale, each note damped ~20 ms before the next pluck\n");
	const long len = scale_buf(0.3f);
	run_plugin(buf, len, defaults());
	int seq[64], n = 0, gaps = 0, big_bends = 0;
	for (int e = 0; e < nev; ++e) {
		if (is_on(&evs[e]) && n < 64) seq[n++] = evs[e].m[1];
		if (is_off(&evs[e])) {
			/* a note-off not immediately followed by a note-on is a gap */
			bool replaced = e + 1 < nev && evs[e + 1].t == evs[e].t;
			for (int f = e + 1; f < nev && evs[f].t == evs[e].t; ++f) replaced = replaced || is_on(&evs[f]);
			if (!replaced) gaps++;
		}
		/* 15 cents: the synthetic string's inharmonicity puts low notes up to
		 * ~11 cents sharp, steadily; the wobble around note changes this
		 * guards against was 30-50 cents. */
		if (is_bend(&evs[e]) && abs((evs[e].m[1] | (evs[e].m[2] << 7)) - 8192) > 614) big_bends++;
	}
	bool same = n == NSCALE;
	for (int k = 0; same && k < n; ++k) same = seq[k] == SCALE[k];
	printf("  note-ons:");
	for (int k = 0; k < n; ++k) printf(" %d", seq[k]);
	printf("\n");
	check(same, "exactly the 15 scale notes, in order");
	check(gaps == 1, "each note replaces the last with no gap (one note-off at the very end)");
	check(big_bends == 0, "no pitch bend over 15 cents");
}

/* Hold Mode = Stack: build A major one note at a time (A1, C#2, E2), pluck
 * A1 again (already sounding: nothing sent), then A2, which drops the oldest
 * note (A1) to stay at three. Release ends the rest. No pitch bend. */
static void test_stack(void)
{
	printf("\nHold Mode = Stack: A1, C#2, E2, A1 again, A2, then Release\n");
	Controls c = defaults();
	c.hold = 1.0f;
	c.hold_mode = 2.0f; /* Stack */
	static const int seq[] = { 33, 37, 40, 33, 45 };
	const long step = FS * 2 / 5, t0 = FS / 5, len = FS * 3;
	memset(buf, 0, sizeof(buf));
	for (int k = 0; k < 5; ++k)
		pluck(buf, t0 + k * step, FS * 7 / 20, seq[k], 0.3f, &MIXES[0], 0, 0, 0);
	release_at = FS * 5 / 2;
	run_plugin(buf, len, c);
	release_at = -1;

	/* Release takes effect at the start of the block it is pressed in. */
	const long rel_t = (FS * 5 / 2) / BLOCK * BLOCK;
	int ons[8], non = 0, offs_before = 0, off_dropped = 0, offs_after = 0, bends = 0;
	for (int e = 0; e < nev; ++e) {
		if (is_on(&evs[e]) && non < 8) ons[non++] = evs[e].m[1];
		if (is_off(&evs[e])) {
			if (evs[e].t < rel_t) { offs_before++; if (evs[e].m[1] == 33) off_dropped++; }
			else offs_after++;
		}
		if (is_bend(&evs[e]) && (evs[e].m[1] | (evs[e].m[2] << 7)) != 8192) bends++;
	}
	printf("  note-ons:");
	for (int k = 0; k < non; ++k) printf(" %d", ons[k]);
	printf("; note-offs before Release %d, after %d\n", offs_before, offs_after);
	check(non == 4 && ons[0] == 33 && ons[1] == 37 && ons[2] == 40 && ons[3] == 45,
	      "A1, C#2, E2, A2 (the repeated A1 sends nothing)");
	check(offs_before == 1 && off_dropped == 1, "only A1 ends before Release, dropped when A2 makes four");
	check(offs_after == 3, "Release ends the three stacked notes");
	check(bends == 0, "no pitch bend while stacking");
}

int main(void)
{
	printf("JTBassMIDI offline tracking test (%d Hz, %d-frame blocks)\n", FS, BLOCK);
	printf("Latency excludes audio interface and buffer latency (~3-6 ms on a pi-Stomp).\n");
	test_strings();
	test_legato();
	test_hammer_on();
	test_light_pluck_and_lift();
	test_fingerstyle_scale();
	test_bend();
	test_velocity();
	test_hold_and_transpose();
	test_release();
	test_latch();
	test_stack();
	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
