#include <gtest/gtest.h>
#include <tether/base64.hpp>
#include <string>
#include <vector>

TEST(Base64Test, EncodeSimple) {
    std::string text = "hello world";
    std::string enc = tether::base64_encode(reinterpret_cast<const unsigned char*>(text.data()), text.size());
    EXPECT_EQ(enc, "aGVsbG8gd29ybGQ=");
}

TEST(Base64Test, DecodeSimple) {
    std::string enc = "aGVsbG8gd29ybGQ=";
    std::vector<unsigned char> dec = tether::base64_decode(enc);
    std::string dec_str(dec.begin(), dec.end());
    EXPECT_EQ(dec_str, "hello world");
}

TEST(Base64Test, DecodeWithPadding) {
    // hello -> aGVsbG8=
    std::string enc = "aGVsbG8=";
    std::vector<unsigned char> dec = tether::base64_decode(enc);
    std::string dec_str(dec.begin(), dec.end());
    EXPECT_EQ(dec_str, "hello");
}

TEST(Base64Test, EdgeCases) {
    EXPECT_EQ(tether::base64_encode(nullptr, 0), "");
    EXPECT_TRUE(tether::base64_decode("").empty());
}

TEST(Base64Test, EncodeVector) {
    std::vector<unsigned char> data = {'h', 'i'};
    std::string enc = tether::base64_encode(data);
    EXPECT_EQ(enc, "aGk=");
}

TEST(Base64Test, RoundTripBinary) {
    std::vector<unsigned char> original = {0x00, 0xFF, 0x01, 0x02, 0x03};
    std::string enc = tether::base64_encode(original);
    auto dec = tether::base64_decode(enc);
    EXPECT_EQ(dec, original);
}

TEST(Base64Test, SingleCharEncode) {
    std::string enc = tether::base64_encode(reinterpret_cast<const unsigned char*>("a"), 1);
    EXPECT_EQ(enc, "YQ==");
}

TEST(Base64Test, TwoCharEncode) {
    std::string enc = tether::base64_encode(reinterpret_cast<const unsigned char*>("ab"), 2);
    EXPECT_EQ(enc, "YWI=");
}

TEST(Base64Test, DecodeInvalidChars) {
    auto dec = tether::base64_decode("!!!");
    EXPECT_TRUE(dec.empty());
}

// Every byte value must survive a round trip. High bytes (>0x7F) become negative
// chars in the encoded buffer and used to reach isalnum() as negative ints.
TEST(Base64Test, RoundTripAllByteValues) {
    std::vector<unsigned char> original(256);
    for (int i = 0; i < 256; ++i) {
        original[i] = static_cast<unsigned char>(i);
    }

    std::string enc = tether::base64_encode(original);
    EXPECT_EQ(enc.size(), 344u); // ceil(256/3)*4
    EXPECT_EQ(tether::base64_decode(enc), original);
}

TEST(Base64Test, RoundTripEveryLengthUpTo64) {
    std::vector<unsigned char> data;
    for (size_t len = 0; len <= 64; ++len) {
        data.assign(len, 0);
        for (size_t i = 0; i < len; ++i) {
            data[i] = static_cast<unsigned char>((i * 37 + 11) & 0xFF);
        }
        std::string enc = tether::base64_encode(data);
        EXPECT_EQ(tether::base64_decode(enc), data) << "length " << len;
    }
}

TEST(Base64Test, DecodeStopsAtFirstInvalidByte) {
    // Decoding is best-effort: it halts at the first non-base64 byte rather than
    // producing garbage for the remainder.
    auto dec = tether::base64_decode("aGVsbG8=trailing junk");
    std::string dec_str(dec.begin(), dec.end());
    EXPECT_EQ(dec_str, "hello");
}

TEST(Base64Test, DecodeHandlesHighBitBytesWithoutCrashing) {
    std::string junk;
    junk.push_back(static_cast<char>(0x80));
    junk.push_back(static_cast<char>(0xFF));
    junk.push_back(static_cast<char>(0xC3));
    EXPECT_TRUE(tether::base64_decode(junk).empty());
}

TEST(Base64Test, RoundTripLargeChunk) {
    // Matches the 512 KB chunk size Client::send_file uses.
    std::vector<unsigned char> data(512 * 1024);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<unsigned char>(i & 0xFF);
    }
    std::string enc = tether::base64_encode(data);
    EXPECT_EQ(tether::base64_decode(enc), data);
}
