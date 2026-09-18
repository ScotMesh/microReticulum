// The neighbour-stats scan used to run on every jobs() tick — four times a
// second — walking the whole stats map and allocating a candidate vector and a
// hex string per neighbour per pass. On a 140 KB TLSF pool that churn
// fragmented the heap until a 64-byte allocation failed (soak run 7: 7,082
// scans and 13,166 skip traces in one hour, dead in thirty).
//
// It is now gated on an interval. Proving that on the air needs the node to
// forward a packet, which a leaf node may not do for hours, so prove it here
// instead: run the real jobs() loop with the gate open and with it closed, and
// count the scans each way. Transport::neighbor_scans() counts invocations at
// the top of the scan, before the empty-map return, so what is measured is how
// often the scan runs — which is exactly what the gate changes.

#include <unity.h>

#include <microStore/Adapters/UniversalFileSystem.h>

#include "microReticulum.h"

#include <stdio.h>
#include <time.h>

namespace {

RNS::Reticulum test_reticulum({RNS::Type::NONE});
bool rns_initialized = false;

void sleep_seconds(double seconds) {
	timespec time;
	time.tv_sec = (time_t)seconds;
	time.tv_nsec = (long)((seconds - (double)time.tv_sec) * 1000000000.0);
	::nanosleep(&time, nullptr);
}

void initRNS() {
	if (rns_initialized) return;
	RNS::loglevel(RNS::LOG_WARNING);   // the scan traces are the point, not the noise

	RNS::Transport::path_table_maxsize(50);
	RNS::Transport::announce_table_maxsize(50);
	RNS::Transport::hashlist_maxsize(50);
	RNS::Transport::max_pr_tags(32);
	RNS::Identity::known_destinations_maxsize(50);

	microStore::FileSystem filesystem{microStore::Adapters::UniversalFileSystem()};
	filesystem.init();
	RNS::Utilities::OS::register_filesystem(filesystem);

	RNS::Bytes transport_prv;
	transport_prv.assignHex("BABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABE"
	                        "BABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABE");
	RNS::Identity transport_identity(false);
	transport_identity.load_private_key(transport_prv);
	RNS::Transport::identity(transport_identity);

	test_reticulum = RNS::Reticulum();
	// The three conditions the scan is gated behind in jobs().
	test_reticulum.transport_enabled(true);
	RNS::Reticulum::neighbor_probing_enabled(true);
	RNS::Reticulum::probe_destination_enabled(true);
	test_reticulum.start();

	rns_initialized = true;
}

// Run the real loop for a while and report how many scans happened.
uint32_t scans_over(double seconds, float interval) {
	RNS::Transport::neighbor_scan_interval(interval);
	const uint32_t before = RNS::Transport::neighbor_scans();
	const double deadline = RNS::Utilities::OS::time() + seconds;
	while (RNS::Utilities::OS::time() < deadline) {
		test_reticulum.loop();
		sleep_seconds(0.01);
	}
	return RNS::Transport::neighbor_scans() - before;
}

// jobs() itself runs on a 250 ms tick, so this is the ceiling on scans.
const double RUN_SECONDS = 6.0;
const uint32_t TICKS = (uint32_t)(RUN_SECONDS / 0.25);

} // namespace

void setUp(void) { initRNS(); }
void tearDown(void) {}

// With the gate open the scan runs on every jobs() tick — the old behaviour,
// and the control that shows the gate is what makes the difference.
void test_scan_runs_every_tick_when_ungated() {
	const uint32_t scans = scans_over(RUN_SECONDS, 0.0f);
	printf("ungated: %u scans in %.0f s (%u ticks available)\n", scans, RUN_SECONDS, TICKS);
	TEST_ASSERT_GREATER_THAN_UINT32(TICKS / 2, scans);
}

// Gated at 15 seconds, a 6-second run may catch the one scan that falls due.
void test_scan_is_gated_by_interval() {
	const uint32_t scans = scans_over(RUN_SECONDS, 15.0f);
	printf("gated at 15 s: %u scans in %.0f s\n", scans, RUN_SECONDS);
	TEST_ASSERT_LESS_OR_EQUAL_UINT32(2, scans);
}

// The claim is a rate reduction of more than an order of magnitude, so measure
// both arms back to back and compare them rather than trusting either alone.
void test_gate_cuts_the_scan_rate_by_an_order_of_magnitude() {
	const uint32_t ungated = scans_over(RUN_SECONDS, 0.0f);
	const uint32_t gated = scans_over(RUN_SECONDS, 15.0f);
	printf("ungated %u vs gated %u over %.0f s each\n", ungated, gated, RUN_SECONDS);
	TEST_ASSERT_GREATER_THAN_UINT32(gated * 10, ungated);
}

// A gate that drifted would be worse than none: confirm it actually fires on
// schedule rather than never, by running past two intervals of a short one.
void test_scan_fires_on_schedule() {
	const uint32_t scans = scans_over(RUN_SECONDS, 1.0f);
	printf("gated at 1 s: %u scans in %.0f s\n", scans, RUN_SECONDS);
	TEST_ASSERT_GREATER_OR_EQUAL_UINT32(3, scans);
	TEST_ASSERT_LESS_OR_EQUAL_UINT32(8, scans);
}

int main(void) {
	UNITY_BEGIN();
	RUN_TEST(test_scan_runs_every_tick_when_ungated);
	RUN_TEST(test_scan_is_gated_by_interval);
	RUN_TEST(test_gate_cuts_the_scan_rate_by_an_order_of_magnitude);
	RUN_TEST(test_scan_fires_on_schedule);
	return UNITY_END();
}
