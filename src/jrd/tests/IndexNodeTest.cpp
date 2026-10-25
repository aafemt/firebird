#include "firebird.h"
#include "boost/test/unit_test.hpp"
#include "../jrd/ods.h"
#include "../jrd/RecordNumber.h"
#include "../jrd/btn.h"

using namespace Firebird;
using namespace Jrd;

BOOST_AUTO_TEST_SUITE(EngineSuite)
BOOST_AUTO_TEST_SUITE(IndexNodeSuite)


BOOST_AUTO_TEST_SUITE(KeyFitsTests)

static IndexNode makeNode(USHORT prefix, USHORT length)
{
	IndexNode node;
	node.nodePointer = nullptr;
	node.prefix = prefix;
	node.length = length;
	node.pageNumber = 0;
	node.data = nullptr;
	node.recordNumber = RecordNumber(1);
	node.isEndBucket = false;
	node.isEndLevel = false;
	return node;
}

BOOST_AUTO_TEST_CASE(KeyFitsExact)
{
	BOOST_TEST(makeNode(0, 0).keyFits(0));
	BOOST_TEST(makeNode(10, 5).keyFits(15));		// prefix + length == size
	BOOST_TEST(makeNode(0, 7).keyFits(7));
}

BOOST_AUTO_TEST_CASE(KeyFitsTooLarge)
{
	BOOST_TEST(!makeNode(10, 5).keyFits(14));		// one byte short
	BOOST_TEST(!makeNode(3, 0).keyFits(2));		// prefix alone overflows
	BOOST_TEST(!makeNode(0xFFFF, 0xFFFF).keyFits(0x1000));
	BOOST_TEST(!makeNode(0, 1).keyFits(0));
}

BOOST_AUTO_TEST_CASE(KeyFitsRolloverGuard)
{
	// length near USHORT_MAX must not pass through an overflowing sum
	BOOST_TEST(!makeNode(1, 0xFFFF).keyFits(0xFFFF));
	BOOST_TEST(makeNode(1, 0xFFFE).keyFits(0xFFFF));
}

BOOST_AUTO_TEST_SUITE_END()	// KeyFitsTests


BOOST_AUTO_TEST_SUITE_END()	// IndexNodeSuite
BOOST_AUTO_TEST_SUITE_END()	// EngineSuite
