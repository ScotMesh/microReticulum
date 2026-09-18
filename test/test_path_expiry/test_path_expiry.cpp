// Paths are the P1's largest tenant: each one costs a path-store index entry
// plus a _path_states entry, about 140 bytes, and the node died with its table
// pegged at 500 of them. Three things now bound that table, and all three are
// measured in hours or days — so rather than wait one out on hardware, move
// OS::_time_offset forward and watch jobs() do its work.
//
//   1. jobs() expires paths past their _expires. The port had lost Python's
//      sweep entirely: microStore only checks its TTL lazily on get(), so an
//      expired record kept its RAM index slot until something asked for it.
//   2. A path learned over a radio expires as roaming (6 h), not as backbone
//      (a week). LoRa nodes are portable; a week of stale entries is a week of
//      index slots on the node least able to spare them.
//   3. cull_announce_table trims to the announce limit, not the path limit.

#include <unity.h>

#include <microStore/Adapters/UniversalFileSystem.h>

#include "microReticulum.h"

#include <stdio.h>
#include <string>

namespace {

// A loopback interface that only ever receives. Bitrate is what decides
// whether Transport treats a path learned here as radio or as wire.
class TestInterface : public RNS::InterfaceImpl {
public:
	TestInterface(const char* name, uint32_t bitrate) : RNS::InterfaceImpl(name) {
		_IN = true;
		_OUT = false;
		_bitrate = bitrate;
	}
	virtual ~TestInterface() { _name = "(deleted)"; }
	virtual bool send_outgoing(const RNS::Bytes& data) { return true; }
	virtual void handle_incoming(const RNS::Bytes& data) {
		try {
			InterfaceImpl::handle_incoming(data);
		}
		catch (const std::exception& e) {
			ERRORF("handle_incoming: exception: %s", e.what());
		}
	}
};

RNS::Reticulum test_reticulum({RNS::Type::NONE});
// 1.76 kbps is the P1's own RNode at SF9/125 kHz; 10 Mbps stands in for the
// backbone TCP interface.
RNS::Interface radio_interface(new TestInterface("RadioInterface", 1760));
RNS::Interface wire_interface(new TestInterface("WireInterface", 10000000));
bool rns_initialized = false;

void initRNS() {
	if (rns_initialized) return;
	RNS::loglevel(RNS::LOG_WARNING);

	RNS::Transport::path_table_maxsize(50);
	RNS::Transport::announce_table_maxsize(50);
	RNS::Transport::hashlist_maxsize(50);
	RNS::Transport::max_pr_tags(32);
	RNS::Identity::known_destinations_maxsize(50);

	microStore::FileSystem filesystem{microStore::Adapters::UniversalFileSystem()};
	filesystem.init();
	RNS::Utilities::OS::register_filesystem(filesystem);

	RNS::Transport::register_interface(radio_interface);
	RNS::Transport::register_interface(wire_interface);

	RNS::Bytes transport_prv;
	transport_prv.assignHex("BABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABE"
	                        "BABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABEBABE");
	RNS::Identity transport_identity(false);
	transport_identity.load_private_key(transport_prv);
	RNS::Transport::identity(transport_identity);

	test_reticulum = RNS::Reticulum();
	test_reticulum.transport_enabled(true);
	test_reticulum.start();
	rns_initialized = true;
}

// Announce a fresh destination into the given interface, so Transport learns a
// path to it exactly as it would from a real neighbour, and return its hash.
RNS::Bytes learn_path_via(RNS::Interface& interface, const char* tag) {
	RNS::Identity id(true);
	RNS::Destination dest(id, RNS::Type::Destination::IN,
		RNS::Type::Destination::SINGLE, "test", tag);
	dest.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);

	RNS::Packet announce = dest.announce(RNS::bytesFromString(tag), false,
		{RNS::Type::NONE}, {RNS::Type::NONE}, false);
	announce.pack();
	// Otherwise the announce looks like our own and is never learned as a path.
	RNS::Transport::deregister_destination(dest);

	interface.handle_incoming(announce.raw());
	for (int i = 0; i < 8; i++) test_reticulum.loop();
	return dest.hash();
}

// Move time forward and let jobs() run its periodic work at the new time.
void advance_hours(double hours) {
	RNS::Utilities::OS::setTimeOffset(
		RNS::Utilities::OS::getTimeOffset() + (uint64_t)(hours * 3600.0 * 1000.0));
	for (int i = 0; i < 12; i++) test_reticulum.loop();
}

const double HOURS_PER_WEEK = 24.0 * 7.0;

} // namespace

void setUp(void) { initRNS(); }
// The offset is never wound back: putting the clock into the past stops every
// periodic job in jobs() from firing until real time catches up, which cost an
// afternoon to work out the first time.
void tearDown(void) {}

// The harness itself has to work before any claim about expiry means anything.
void test_announce_is_learned_as_a_path() {
	RNS::Bytes dest = learn_path_via(wire_interface, "learned");
	TEST_ASSERT_TRUE_MESSAGE(RNS::Transport::has_path(dest),
		"an announce from a remote destination should be learned as a path");
}

// The sweep that this port had lost: without it an expired record keeps its
// index slot in RAM until something happens to ask for it.
void test_expired_path_is_swept_by_jobs() {
	RNS::Bytes dest = learn_path_via(wire_interface, "sweep");
	TEST_ASSERT_TRUE(RNS::Transport::has_path(dest));

	advance_hours(HOURS_PER_WEEK + 24.0);   // past PATHFINDER_E
	TEST_ASSERT_FALSE_MESSAGE(RNS::Transport::has_path(dest),
		"a path past its expiry should be removed by jobs()");
}

// The heart of it: same code, same announce, different interface bitrate.
// Six hours in, the radio path is gone and the backbone path is not.
void test_radio_paths_expire_as_roaming_and_wire_paths_do_not() {
	RNS::Bytes over_radio = learn_path_via(radio_interface, "portable");
	RNS::Bytes over_wire = learn_path_via(wire_interface, "backbone");
	TEST_ASSERT_TRUE(RNS::Transport::has_path(over_radio));
	TEST_ASSERT_TRUE(RNS::Transport::has_path(over_wire));

	advance_hours(7.0);   // past ROAMING_PATH_TIME (6 h), far short of a week

	TEST_ASSERT_FALSE_MESSAGE(RNS::Transport::has_path(over_radio),
		"a path learned over a radio should expire as roaming, within six hours");
	TEST_ASSERT_TRUE_MESSAGE(RNS::Transport::has_path(over_wire),
		"a path learned over a wire should still be held after seven hours");
}

// A cull that trimmed to the wrong table's limit would either strip the
// announce table bare or leave it over its cap, depending on which limit
// happened to be larger.
void test_announce_table_culls_to_its_own_limit() {
	RNS::Transport::announce_table_maxsize(4);
	RNS::Transport::path_table_maxsize(50);   // deliberately much larger

	for (int i = 0; i < 12; i++) {
		learn_path_via(wire_interface, ("cull" + std::to_string(i)).c_str());
	}
	for (int i = 0; i < 8; i++) test_reticulum.loop();

	const size_t held = RNS::Transport::announce_table().size();
	printf("announce table holds %zu with a limit of 4 (path limit 50)\n", held);
	TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(5, held,
		"the announce table should be trimmed to its own limit, not the path table's");

	RNS::Transport::announce_table_maxsize(50);
}

int main(void) {
	UNITY_BEGIN();
	RUN_TEST(test_announce_is_learned_as_a_path);
	RUN_TEST(test_radio_paths_expire_as_roaming_and_wire_paths_do_not);
	RUN_TEST(test_expired_path_is_swept_by_jobs);
	RUN_TEST(test_announce_table_culls_to_its_own_limit);
	return UNITY_END();
}
