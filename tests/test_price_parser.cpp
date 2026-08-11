#include <gtest/gtest.h>
#include "utils/price_parser.hpp"

// -----------------------------------------------------------------------------
// Price parsing: basic conversions
// -----------------------------------------------------------------------------


// Want to call individual test cases ? 
// run this -> ./test_price_parser --gtest_filter=PriceParserTest.ParsePriceToTick_Basic
TEST(PriceParserTest, ParsePriceToTick_Basic) {
    EXPECT_EQ(parsePriceToTick("586.18", 2), 58618);
    EXPECT_EQ(parsePriceToTick("100", 2), 10000);
    EXPECT_EQ(parsePriceToTick("0.00001", 5), 1);
    EXPECT_EQ(parsePriceToTick("1.23456", 5), 123456);
}

// -----------------------------------------------------------------------------
// Price parsing: trailing zeros should not change the represented value
// -----------------------------------------------------------------------------

TEST(PriceParserTest, ParsePriceToTick_TrailingZeros) {
    EXPECT_EQ(parsePriceToTick("586.18", 2), 58618);
    EXPECT_EQ(parsePriceToTick("586.1800", 2), 58618);
    EXPECT_EQ(parsePriceToTick("586.18000000", 2), 58618);

    EXPECT_EQ(parsePriceToTick("100", 2), 10000);
    EXPECT_EQ(parsePriceToTick("100.0", 2), 10000);
    EXPECT_EQ(parsePriceToTick("100.00", 2), 10000);
}

// -----------------------------------------------------------------------------
// Price parsing: fewer decimal digits are padded with zeros
// -----------------------------------------------------------------------------

TEST(PriceParserTest, ParsePriceToTick_PadsDecimalPart) {
    EXPECT_EQ(parsePriceToTick("1.2", 2), 120);
    EXPECT_EQ(parsePriceToTick("1.23", 4), 12300);
    EXPECT_EQ(parsePriceToTick("1", 4), 10000);
}

// -----------------------------------------------------------------------------
// Price parsing: extra decimal digits are truncated
//
// This reflects the current implementation:
// while (dec_count > decimals)
//     dec_part /= 10;
// -----------------------------------------------------------------------------

TEST(PriceParserTest, ParsePriceToTick_TruncatesExtraPrecision) {
    EXPECT_EQ(parsePriceToTick("1.234", 2), 123);
    EXPECT_EQ(parsePriceToTick("1.239", 2), 123);
    EXPECT_EQ(parsePriceToTick("1.23456789", 5), 123456);
}

// -----------------------------------------------------------------------------
// Price parsing: zero
// -----------------------------------------------------------------------------

TEST(PriceParserTest, ParsePriceToTick_Zero) {
    EXPECT_EQ(parsePriceToTick("0", 2), 0);
    EXPECT_EQ(parsePriceToTick("0.0", 2), 0);
    EXPECT_EQ(parsePriceToTick("0.00", 2), 0);
    EXPECT_EQ(parsePriceToTick("0.00000", 5), 0);
}

// -----------------------------------------------------------------------------
// Price parsing: very large
// -----------------------------------------------------------------------------

TEST(PriceParserTest, ParsePriceToTick_Large) {
    EXPECT_EQ(parsePriceToTick("100000.00", 2), 10000000);
}

// -----------------------------------------------------------------------------
// Quantity parsing
// -----------------------------------------------------------------------------

TEST(PriceParserTest, ParseQuantityToScaled_Basic) {
    EXPECT_EQ(parseQuantityToScaled("39.71100000", 3), 39711);
    EXPECT_EQ(parseQuantityToScaled("100", 0), 100);
    EXPECT_EQ(parseQuantityToScaled("1.234", 3), 1234);
    EXPECT_EQ(parseQuantityToScaled("0.00001", 5), 1);
}

TEST(PriceParserTest, ParseQuantityToScaled_TrailingZeros) {
    EXPECT_EQ(parseQuantityToScaled("39.711", 3), 39711);
    EXPECT_EQ(parseQuantityToScaled("39.711000", 3), 39711);
    EXPECT_EQ(parseQuantityToScaled("100", 2), 10000);
    EXPECT_EQ(parseQuantityToScaled("100.00", 2), 10000);
}

TEST(PriceParserTest, ParseQuantityToScaled_PadsDecimalPart) {
    EXPECT_EQ(parseQuantityToScaled("1.2", 3), 1200);
    EXPECT_EQ(parseQuantityToScaled("1.23", 4), 12300);
}

TEST(PriceParserTest, ParseQuantityToScaled_TruncatesExtraPrecision) {
    EXPECT_EQ(parseQuantityToScaled("1.2345", 3), 1234);
    EXPECT_EQ(parseQuantityToScaled("39.711999", 3), 39711);
}

TEST(PriceParserTest, ParseQuantityToScaled_Zero) {
    EXPECT_EQ(parseQuantityToScaled("0", 3), 0);
    EXPECT_EQ(parseQuantityToScaled("0.000", 3), 0);
}