// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// SEQUENTIA: reading parent-chain blocks for the PoS checkpoints committed in
// them.
//
// The scan used to ask bitcoind for a block as JSON with every transaction
// decoded, and pick the output scripts out of that. It now asks for the block
// raw and takes it apart here, which is a great deal cheaper -- and which moves
// the parsing from a daemon that has done it correctly for fifteen years into
// this file.
//
// So the first test is not about checkpoints at all. It is the question that
// swap actually raises: does taking a real block apart here see EXACTLY the
// outputs the daemon reported? The block below is a real testnet4 block, and
// the expected scripts are literally what `getblock <hash> 2` returned for it.
// If witness parsing were wrong, or a transaction were skipped, or the outputs
// came out in another order, this is where it shows.

#include <anchor.h>
#include <primitives/bitcoin/block.h>
#include <streams.h>
#include <util/strencodings.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(checkpoint_scan_tests, BasicTestingSetup)

namespace {
// testnet4 block 149795: 8 transactions, 16 outputs, segwit among them.
const char* kRawBlock =
    "00c0b02846bfa54160c6d2f14fdc30bb90e3f96009bb26c959261a47743cb700000000003195788095d4bcea16b10f0f"
    "0bf5685935bc3366073d4e50d38394d0952df8e1cae58d6affff001d00006b8208010000000001010000000000000000"
    "000000000000000000000000000000000000000000000000ffffffff1703234902000454c68d6a0cfbc7816a00000000"
    "00000000ffffffff024887092a0100000016001485e9d16efcd61814f84fd2c8e996768ba43fc31c0000000000000000"
    "266a24aa21a9ed91ec3cf0cc216b1eee2dc9ec13ab4913317b8b427780bf15f3932b4771d3892c012000000000000000"
    "000000000000000000000000000000000000000000000000000000000001000000000101a5b2a037c9cd5964a6f2aa97"
    "5820e9428fa1dc2fecb86624b399484f8d51118b0000000000fdffffff02016035000000000016001403a11ef572d484"
    "a988edf0b2976d7bc6306af37f4504000000000000160014deb34719cb032ef74f552edeb4882e2bb155247c02483045"
    "022100a769b3a46b019ca9b12e2d2c31ff30ce88746a61acce296e89029784da8b0b02022037c19d40e1add0aa1838b0"
    "cb8f296836402316e9aedbc99984eac74d9bb440930121020b3f09dd02d7321a19bfa739954264dd27b2209c54d28144"
    "698aa97fccc44d310000000001000000000101d6d90ff1b68f027daa0cd52ef9a6710c87e09b49ddb0991f88264e8c34"
    "5fcdfc0100000000fdffffff022e975d000000000016001403a11ef572d484a988edf0b2976d7bc6306af37f45040000"
    "00000000160014644bfa0055cdb8ad17c93e4a1b152bcdcc51f179024730440220370fbb8fd949d56e0017b5e7990552"
    "e7772996a6ba97b2955ca551705e4db45002201833873db63463069f6f792fc3d880bc83100b55733aff11eb0ca67215"
    "405d310121020b3f09dd02d7321a19bfa739954264dd27b2209c54d28144698aa97fccc44d3100000000010000000001"
    "01d45c593056f8370fbfcc63a4675f31bb0862f496803ea01bb14bf3ae1916091d0000000000fdffffff0216c75d0000"
    "00000016001403a11ef572d484a988edf0b2976d7bc6306af37f4504000000000000160014c1ece843cb610a903bdb83"
    "42652376e8278188d80247304402200677dc236d0dcba6dca0f552c320034e24572f1fb56ef0cfa6add6e7da37d9d202"
    "2077b913b2b577ffe3dbb2e5cb8b54ceda2c9d6a3d653ae183ed6c25f43d10c0010121020b3f09dd02d7321a19bfa739"
    "954264dd27b2209c54d28144698aa97fccc44d310000000001000000000101369d58a2bdfe10c94ced71dc634b8634a5"
    "3ee13258d5c6a76dbeee05c4aad4c50000000000fdffffff02a6845c000000000016001403a11ef572d484a988edf0b2"
    "976d7bc6306af37f9f610000000000001976a914f7d68541877279d478f86e4655595559042bcba688ac024730440220"
    "770db413d30ff9c40c61268ae42a87317f5d9f55b16e443ab428b0f16f21a4c70220256173488f027d9b84d76dfb156d"
    "767177f7b55ac9af581f061a6666b285f9d50121020b3f09dd02d7321a19bfa739954264dd27b2209c54d28144698aa9"
    "7fccc44d310000000001000000000102c488c1857a744014b6bd517cf93ea492b54f33c8dc9db8e504b9371fa69ed364"
    "0b00000000fdffffffc488c1857a744014b6bd517cf93ea492b54f33c8dc9db8e504b9371fa69ed3640c00000000fdff"
    "ffff02e2b201000000000017a914c2b0fa0e720e1fcc6c10c6e0f0e9aff14739368f87c4ac0100000000002200202074"
    "5ba04f497b4860f91950ad225122e6faa852b71f82e79f59ebdd673ad6ef040048304502210084ddd582b1ab35bdc490"
    "020c42f32841355184d8fafb4d15894d4824a62500cd022040d97ee60a1bb84a9001cadc2a1db6aeba00605af29b2491"
    "62b378f47c996212014830450221008a6754a75aeb9345dd0c1ddd896fe7c00bd87ecb9da1bfd5bbf94d0ae291a69402"
    "202bdaefe26c839e8c458d42c3d2ece6b5aacc57aaf9b74c80dca9f3eb5b0eea650169522102f15e2b3b890b5859f097"
    "9fdb18bb407eae599b9715ce3feddc408704e52504da21033f20e9950682ccb53a4c730900c6451c8d6629b45207bd1f"
    "37654f9e205409372103762bd1607a42eec827458ee7abc1b39aada27bc718cae3941bcd8dfb911e8dcc53ae04004730"
    "440220599a6df2f3a653e2776c4861f855c79440b3c92aa87d0bae87ca4436f6f384eb022031bcf85c2bdb5aa9f87466"
    "375d501c7759937bb35176e4cacacd9f35fafcd2a1014730440220454c3442d9ceddeacb06680841b79bf9d55b6cd0c9"
    "4f93d854567fb8d82ed863022074df064b0d50945734046f119a6953104fb33e1e09d00ae9e7c843bfcd37a4ea016952"
    "2102882ca17e1f8b7199227a1f0bfa940f2e2dfb7fa5d275d4badae98664170a1135210307410663eed57f8690cfcfcb"
    "52a5e8999023d6358fd8d5ddaaf0eb377bd6f7b0210317f1f526a44f909096167efc7587476571c84ebf5790f329a23e"
    "4f391ea2434053ae00000000020000000001011ce42aedfb9c881eef3d8f79ae360b752f218d21d3214b9cec250b1e15"
    "7a3ed30100000000fdffffff020000000000000000346a3241434d450201000249228da5ebb2c6eacdc0c00fae730f7c"
    "b0747e52c0bb673feb5503423eb831c7d2440000000000000000c73d38010000000016001444ba85bd55937066d68773"
    "150b5b882f12a5af720247304402206111a35e9855861472b740bb4726b64a4192378cd494ae8ffd85426b66b6b29102"
    "2035bfbbc84f00240a3454cb04b06a7e0934bbd96dc2d06861c3596f1ac6cfbb720121038cce9bfe687e982f99202434"
    "f12d39f1891aff44c79840cb5440289d0c7c950b00000000020000000001017646e51948be75803a6fd784fb8fe7a2ef"
    "2a33364189e8bdc50fe1ec004d764c00000000000100000002d660030000000000160014ff0ec9659ba14c3f95a02121"
    "36966742fa42db8fa0860100000000002251203499b1935a007f78cb65b7bd38a5f22f53f4ce541150fce6958218c165"
    "6590780247304402203d13993f5ccbaf191bd55e0785989f53b0b90d14825135ccd23b4f22b8d228ef022061dc210e23"
    "397684367d496850b63e57c942ee4832140e3b4fe39ed274dcc540012103ff25f9bba61aa453a4e0bd5d7fe1bc3037b2"
    "a7135faeb027a78e7229cbec0f9700000000";

// Every output script in that block, in order, as bitcoind's verbosity-2 JSON
// reported them. Not derived from the code under test.
const std::vector<std::string> kExpectedScripts = {
        "001485e9d16efcd61814f84fd2c8e996768ba43fc31c",
        "6a24aa21a9ed91ec3cf0cc216b1eee2dc9ec13ab4913317b8b427780bf15f3932b4771d3892c",
        "001403a11ef572d484a988edf0b2976d7bc6306af37f",
        "0014deb34719cb032ef74f552edeb4882e2bb155247c",
        "001403a11ef572d484a988edf0b2976d7bc6306af37f",
        "0014644bfa0055cdb8ad17c93e4a1b152bcdcc51f179",
        "001403a11ef572d484a988edf0b2976d7bc6306af37f",
        "0014c1ece843cb610a903bdb8342652376e8278188d8",
        "001403a11ef572d484a988edf0b2976d7bc6306af37f",
        "76a914f7d68541877279d478f86e4655595559042bcba688ac",
        "a914c2b0fa0e720e1fcc6c10c6e0f0e9aff14739368f87",
        "002020745ba04f497b4860f91950ad225122e6faa852b71f82e79f59ebdd673ad6ef",
        "6a3241434d450201000249228da5ebb2c6eacdc0c00fae730f7cb0747e52c0bb673feb5503423eb831c7d2440000000000000000",
        "001444ba85bd55937066d68773150b5b882f12a5af72",
        "0014ff0ec9659ba14c3f95a0212136966742fa42db8f",
        "51203499b1935a007f78cb65b7bd38a5f22f53f4ce541150fce6958218c165659078",
};

Sidechain::Bitcoin::CBlock DeserializeBlock(const std::string& hex)
{
    const std::vector<unsigned char> raw = ParseHex(hex);
    CDataStream ss(raw, SER_NETWORK, PROTOCOL_VERSION);
    Sidechain::Bitcoin::CBlock block;
    ss >> block;
    return block;
}
}  // namespace

BOOST_AUTO_TEST_CASE(a_real_parent_block_parses_to_the_outputs_the_daemon_reported)
{
    const Sidechain::Bitcoin::CBlock block = DeserializeBlock(kRawBlock);

    // The header first: if this is wrong, nothing below means anything.
    BOOST_CHECK_EQUAL(block.GetHash().ToString(),
                      "0000000000d7224412a7751fea935f25d3218e786906d1bde781402b5b4cabef");
    BOOST_CHECK_EQUAL(block.vtx.size(), 8U);

    std::vector<std::string> got;
    for (const auto& tx : block.vtx) {
        BOOST_REQUIRE(tx);
        for (const auto& out : tx->vout) got.push_back(HexStr(out.scriptPubKey));
    }

    BOOST_REQUIRE_EQUAL(got.size(), kExpectedScripts.size());
    for (size_t i = 0; i < got.size(); ++i) {
        BOOST_CHECK_MESSAGE(got[i] == kExpectedScripts[i],
                            "output " << i << " differs from what the daemon reported");
    }
}

BOOST_AUTO_TEST_CASE(a_checkpoint_in_a_parent_block_is_found)
{
    // SEQCKPT || 32-byte block hash || 4-byte height, little-endian, behind an
    // OP_RETURN -- built here rather than borrowed, so the test states the
    // format instead of agreeing with whatever the code happens to do.
    uint256 seq_block;
    for (int i = 0; i < 32; ++i) *(seq_block.begin() + i) = (unsigned char)(i + 1);
    const uint32_t seq_height = 0x01020304;

    std::vector<unsigned char> payload = {'S', 'E', 'Q', 'C', 'K', 'P', 'T'};
    payload.insert(payload.end(), seq_block.begin(), seq_block.end());
    payload.push_back((unsigned char)(seq_height & 0xff));
    payload.push_back((unsigned char)((seq_height >> 8) & 0xff));
    payload.push_back((unsigned char)((seq_height >> 16) & 0xff));
    payload.push_back((unsigned char)((seq_height >> 24) & 0xff));

    Sidechain::Bitcoin::CMutableTransaction mtx;
    mtx.vout.resize(2);
    mtx.vout[0].nValue = 0;
    mtx.vout[0].scriptPubKey = CScript() << OP_RETURN << payload;
    // A second, ordinary output: the scan must walk past it, not stop at it.
    mtx.vout[1].nValue = 1000;
    mtx.vout[1].scriptPubKey = CScript() << OP_TRUE;

    Sidechain::Bitcoin::CBlock block;
    block.vtx.push_back(Sidechain::Bitcoin::MakeTransactionRef(Sidechain::Bitcoin::CTransaction(mtx)));

    uint256 parent_hash;
    for (int i = 0; i < 32; ++i) *(parent_hash.begin() + i) = 0xAB;
    ScanRawBlockForCheckpoints(block, 800123, parent_hash);

    bool found = false;
    for (const PosCheckpoint& c : GetPosCheckpoints()) {
        if (c.seq_hash != seq_block) continue;
        found = true;
        BOOST_CHECK_EQUAL(c.seq_height, seq_height);
        BOOST_CHECK_EQUAL(c.btc_height, 800123);
        BOOST_CHECK_EQUAL(c.btc_hash.ToString(), parent_hash.ToString());
    }
    BOOST_CHECK_MESSAGE(found, "the checkpoint in the block was not recorded");
}

BOOST_AUTO_TEST_CASE(things_that_are_not_checkpoints_are_left_alone)
{
    const size_t before = GetPosCheckpoints().size();

    Sidechain::Bitcoin::CMutableTransaction mtx;
    mtx.vout.resize(3);
    // An OP_RETURN carrying somebody else's data.
    mtx.vout[0].scriptPubKey = CScript() << OP_RETURN << std::vector<unsigned char>{'h', 'e', 'l', 'l', 'o'};
    // The right length, the wrong tag.
    std::vector<unsigned char> wrong_tag(7 + 32 + 4, 0);
    wrong_tag[0] = 'X';
    mtx.vout[1].scriptPubKey = CScript() << OP_RETURN << wrong_tag;
    // A bare OP_RETURN with nothing after it.
    mtx.vout[2].scriptPubKey = CScript() << OP_RETURN;

    Sidechain::Bitcoin::CBlock block;
    block.vtx.push_back(Sidechain::Bitcoin::MakeTransactionRef(Sidechain::Bitcoin::CTransaction(mtx)));

    uint256 parent_hash;
    ScanRawBlockForCheckpoints(block, 800124, parent_hash);
    BOOST_CHECK_EQUAL(GetPosCheckpoints().size(), before);
}

BOOST_AUTO_TEST_SUITE_END()
