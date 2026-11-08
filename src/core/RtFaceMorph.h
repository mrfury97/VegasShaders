#pragma once

// UNOFFICIAL 2026-11-07 faces and lip-sync in the RT scene ([Main.Main.RayTracing] FaceMorphs, docs/next/RT-DETAIL-GAPS.md section 4).
// The RT core poses an actor's skinned partitions from the partition's own vertex buffer (RtCore::AddSkinned -> StageSkinnedGeometry;
// the fork reads the buffer's contents when the frame's staging is flushed). A FaceGen head's expressions and lip-sync are morphs of its
// vertices with no bone moving, so the frames ActorStillSkip / ActorUpdateInterval leave out would keep a talking face's old mouth.
// These are the decisions RtCore makes for a head, without the game's types:
//  - Hash: a sampled hash of a vertex array's positions (the partition's buffer, the geometry data's CPU vertices, its own buffer),
//    the same vertices every frame, so a change of any sampled vertex changes it.
//  - Observe: each frame's three hashes -> did the head change, must it be posed now even when the bones are still (force), and which
//    source its poses read ("feed"): the partition's buffer while it follows the morphs; the CPU vertices (or the data's own buffer)
//    when they change and the partition's buffer does not catch up within kCatchUpFrames frames, kSwitchAfter times (then for good).
//  - PatchPositions: a copy of the partition's vertices with each position taken from the feed through the partition's vertex map.
// tools/nvr-rt-host scene "facemorph" runs these with a real skinned mesh through the fork and the helper. Plain C++ only.

#include <stdint.h>
#include <string.h>

namespace RtFaceMorph {

	constexpr uint32_t kSamples = 256;		// vertices hashed a source (all of them when fewer): a morph of ~100 scattered vertices is missed
											// with probability (1 - 256 / N)^100 (N = 1500: 1e-8)
	constexpr uint32_t kCatchUpFrames = 2;	// the CPU vertices changed: the partition's buffer must change in that frame or the next two
	constexpr uint32_t kSwitchAfter = 2;	// ... or it counts as a miss; this many misses (and more than follows): the poses read the CPU
											// vertices (own buffer) from then on

	// FNV-1a over `bytes` bytes at `positionOffset` of up to `samples` vertices spread evenly over `count` (stride apart)
	inline uint32_t Hash(const uint8_t* base, uint32_t count, uint32_t stride, uint32_t positionOffset, uint32_t bytes = 12u, uint32_t samples = kSamples) {
		uint32_t h = 2166136261u;
		if (!base || !count || !stride || !samples) return h;
		const uint32_t n = count < samples ? count : samples;
		for (uint32_t k = 0; k < n; k++) {
			const uint32_t i = (uint32_t)(((uint64_t)k * count) / n);
			const uint8_t* p = base + (size_t)i * stride + positionOffset;
			for (uint32_t b = 0; b < bytes; b++) { h ^= p[b]; h *= 16777619u; }
		}
		h ^= count; h *= 16777619u;
		return h;
	}

	enum Feed : uint8_t { FeedPartition = 0, FeedCpu = 1, FeedOwn = 2 };
	inline const char* FeedName(int f) { return f == FeedCpu ? "the geometry data's CPU vertices (copied into a buffer of our own)" :
		f == FeedOwn ? "the geometry data's own vertex buffer (copied into a buffer of our own)" : "the partition's vertex buffer (the game's)"; }

	struct Source {
		bool		have = false;		// a hash was read before
		uint32_t	hash = 0;
		uint32_t	changes = 0;		// frames its hash differed from the frame before
		uint32_t	reads = 0;			// frames it was read
	};
	struct Sample {						// this frame's reads (ok false: not there / not readable this frame)
		bool part = false, cpu = false, own = false;
		uint32_t partHash = 0, cpuHash = 0, ownHash = 0;
	};
	struct State {
		Source		part, cpu, own;
		uint32_t	frames = 0;			// frames observed
		uint8_t		feed = FeedPartition;
		uint32_t	waiting = 0;		// frames since a CPU (own) change the partition has not followed yet (0: none pending)
		uint8_t		waitingFor = FeedPartition;	// which source's change is pending
		uint32_t	misses = 0;			// changes the partition's buffer never followed (kCatchUpFrames)
		uint32_t	followed = 0;		// changes it followed
		uint32_t	forced = 0;			// frames Observe asked for a pose
	};
	struct Decision {
		bool		changed = false;	// the feed's vertices differ from the last frame's
		bool		force = false;		// pose now (whatever the bones did); with feed != partition: refresh the copy first
		bool		switched = false;	// the feed changed this frame
		uint8_t		feed = FeedPartition;
	};

	inline bool Step(Source& s, bool ok, uint32_t hash) {
		if (!ok) return false;
		s.reads++;
		const bool changed = s.have && s.hash != hash;
		if (changed) s.changes++;
		s.have = true; s.hash = hash;
		return changed;
	}

	inline Decision Observe(State& s, const Sample& in) {
		s.frames++;
		const bool partChanged = Step(s.part, in.part, in.partHash);
		const bool cpuChanged = Step(s.cpu, in.cpu, in.cpuHash);
		const bool ownChanged = Step(s.own, in.own, in.ownHash);
		Decision d;
		// does the partition's buffer follow the CPU (own) vertices? (only while it is still the feed)
		if (s.feed == FeedPartition) {
			if (s.waiting) {
				if (partChanged) { s.followed++; s.waiting = 0; }
				else if (++s.waiting > kCatchUpFrames) { s.misses++; s.waiting = 0; }
			}
			if (!s.waiting && !partChanged && (cpuChanged || ownChanged)) { s.waiting = 1; s.waitingFor = cpuChanged ? FeedCpu : FeedOwn; }
			else if (partChanged && (cpuChanged || ownChanged)) s.followed++;	// followed in the same frame
			// more misses than follows: the partition's buffer does not carry the morphs (or not while the head is not drawn, e.g. off
			// screen); a stray miss (a small morph none of the partition's sampled vertices saw) against many follows does not switch
			if (s.misses >= kSwitchAfter && s.misses > s.followed) {
				s.feed = s.waitingFor != FeedPartition ? s.waitingFor : (s.cpu.changes ? FeedCpu : FeedOwn);
				if (s.feed == FeedCpu && !in.cpu) s.feed = FeedOwn;
				if (s.feed == FeedOwn && !in.own) s.feed = in.cpu ? FeedCpu : FeedPartition;
				d.switched = s.feed != FeedPartition;
			}
		}
		d.feed = s.feed;
		d.changed = s.feed == FeedPartition ? partChanged : s.feed == FeedCpu ? cpuChanged : ownChanged;
		d.force = d.changed || d.switched;
		if (d.force) s.forced++;
		return d;
	}

	// Before the poses read a copy: does the vertex map (and the layout read from the game's types) put each partition vertex where the
	// feed has it? Of up to `samples` partition vertices, how many lie within `tolerance` (each axis) of their mapped feed vertex; a stale
	// partition differs only where the morph moved it, a wrong map or offset matches next to none. tested = vertices compared.
	inline uint32_t MapAgreement(const uint8_t* part, uint32_t count, uint32_t stride, uint32_t positionOffset, const uint8_t* src, uint32_t srcCount,
		uint32_t srcStride, uint32_t srcOffset, const uint16_t* map, uint32_t& tested, uint32_t samples = kSamples, float tolerance = 0.5f) {
		tested = 0;
		if (!part || !src || !count || !srcCount) return 0;
		const uint32_t n = count < samples ? count : samples;
		uint32_t agree = 0;
		for (uint32_t k = 0; k < n; k++) {
			const uint32_t i = (uint32_t)(((uint64_t)k * count) / n), v = map ? (uint32_t)map[i] : i;
			tested++;
			if (v >= srcCount) continue;
			float a[3], b[3];
			memcpy(a, part + (size_t)i * stride + positionOffset, 12);
			memcpy(b, src + (size_t)v * srcStride + srcOffset, 12);
			const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
			if (dx <= tolerance && dx >= -tolerance && dy <= tolerance && dy >= -tolerance && dz <= tolerance && dz >= -tolerance) agree++;
		}
		return agree;
	}

	// dst: `count` vertices of the partition (stride apart, the position `positionOffset` bytes in, 12 bytes: x y z floats; a float4's w
	// is kept); src: the feed's positions per geometry-data vertex (srcStride apart, x y z floats at srcOffset), srcCount of them;
	// map: partition vertex -> data vertex (null: the same index). False (dst untouched) when the map points past srcCount.
	inline bool PatchPositions(uint8_t* dst, uint32_t count, uint32_t stride, uint32_t positionOffset, const uint8_t* src, uint32_t srcCount,
		uint32_t srcStride, uint32_t srcOffset, const uint16_t* map) {
		if (!dst || !src || !count || !srcCount) return false;
		for (uint32_t i = 0; i < count; i++) if ((map ? (uint32_t)map[i] : i) >= srcCount) return false;
		for (uint32_t i = 0; i < count; i++) {
			const uint32_t v = map ? (uint32_t)map[i] : i;
			memcpy(dst + (size_t)i * stride + positionOffset, src + (size_t)v * srcStride + srcOffset, 12);
		}
		return true;
	}
}
