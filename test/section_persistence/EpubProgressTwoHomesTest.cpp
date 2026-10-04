#include <gtest/gtest.h>

#include "BookContentIdStub.h"
#include "activities/reader/EpubReaderUtils.h"

namespace {
// The fork's second progress home, next to the path-keyed one upstream's
// EpubProgressPersistenceTest covers.
class EpubProgressTwoHomesTest : public testing::Test {
 protected:
  Epub epub{"/books/test.epub", "/cache"};
  void SetUp() override {
    Storage.reset();
    BookContentIdStub::stateDir = "/state";
  }
  void TearDown() override { BookContentIdStub::stateDir.clear(); }
};

TEST_F(EpubProgressTwoHomesTest, EverySaveReachesBothHomes) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10, 123));
  EXPECT_EQ(Storage.writeOpens, 2U);
  EXPECT_EQ(Storage.bytes("/cache/progress.bin"), Storage.bytes("/state/progress.bin"));
}

TEST_F(EpubProgressTwoHomesTest, IdenticalSavesWriteNeitherHome) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10, 123));
  const auto writes = Storage.writeOpens;
  const auto removes = Storage.removes;
  const auto renames = Storage.renames;
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10, 123));
  EXPECT_EQ(Storage.writeOpens, writes);
  EXPECT_EQ(Storage.removes, removes);
  EXPECT_EQ(Storage.renames, renames);
}

TEST_F(EpubProgressTwoHomesTest, OnlyTheHomeThatFellBehindIsRewritten) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10, 123));
  // A move or a cache clear leaves the content-keyed copy alone and drops the other.
  Storage.remove("/cache/progress.bin");
  const auto writes = Storage.writeOpens;
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10, 123));
  EXPECT_EQ(Storage.writeOpens, writes + 1);
  EXPECT_EQ(Storage.bytes("/cache/progress.bin"), Storage.bytes("/state/progress.bin"));
}

TEST_F(EpubProgressTwoHomesTest, TheFurtherCopyWinsOnLoad) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 3, 4, 10, 900));
  const auto further = Storage.bytes("/state/progress.bin");
  // Stock firmware only writes the path-keyed copy; an older position there must not rewind.
  BookContentIdStub::stateDir.clear();
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10, 100));
  BookContentIdStub::stateDir = "/state";
  EXPECT_EQ(Storage.bytes("/state/progress.bin"), further);

  EpubReaderUtils::Progress progress;
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, progress));
  EXPECT_EQ(progress.spineIndex, 3);
  EXPECT_EQ(progress.pageNumber, 4);
}
}  // namespace
