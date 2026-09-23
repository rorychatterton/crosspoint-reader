#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "SyncDecision.h"

using koreader_sync::decideSmart;
using koreader_sync::preferAlternate;
using koreader_sync::SmartAction;

namespace {
constexpr float kEps = 0.001f;
constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
}  // namespace

// --- decideSmart -----------------------------------------------------------

TEST(DecideSmart, EqualIsAlreadySynced) { EXPECT_EQ(decideSmart(0.5f, 0.5f, kEps), SmartAction::AlreadySynced); }

TEST(DecideSmart, ZeroEqualIsAlreadySynced) { EXPECT_EQ(decideSmart(0.0f, 0.0f, kEps), SmartAction::AlreadySynced); }

TEST(DecideSmart, LocalSlightlyAheadWithinEpsilonIsAlreadySynced) {
  EXPECT_EQ(decideSmart(0.5f + 0.0009f, 0.5f, kEps), SmartAction::AlreadySynced);
}

TEST(DecideSmart, LocalSlightlyBehindWithinEpsilonIsAlreadySynced) {
  EXPECT_EQ(decideSmart(0.5f - 0.0009f, 0.5f, kEps), SmartAction::AlreadySynced);
}

TEST(DecideSmart, LocalAheadBeyondEpsilonUploads) {
  EXPECT_EQ(decideSmart(0.5f + 0.002f, 0.5f, kEps), SmartAction::UploadLocal);
}

TEST(DecideSmart, RemoteAheadBeyondEpsilonApplies) {
  EXPECT_EQ(decideSmart(0.5f - 0.002f, 0.5f, kEps), SmartAction::ApplyRemote);
}

TEST(DecideSmart, EpsilonIsInclusive) {
  // A delta exactly at the tolerance still counts as the same position.
  EXPECT_EQ(decideSmart(0.75f, 0.5f, 0.25f), SmartAction::AlreadySynced);
  EXPECT_EQ(decideSmart(0.25f, 0.5f, 0.25f), SmartAction::AlreadySynced);
}

TEST(DecideSmart, DefaultEpsilonMatchesActivityTolerance) {
  // The activity passes 0.001f explicitly; the default must agree with it.
  EXPECT_EQ(decideSmart(0.5f + 0.0009f, 0.5f), SmartAction::AlreadySynced);
  EXPECT_EQ(decideSmart(0.5f + 0.002f, 0.5f), SmartAction::UploadLocal);
}

TEST(DecideSmart, NanRemoteUploadsLocal) {
  // A remote record whose percentage is not a number is not comparable; the
  // only trustworthy position is the local one, so it wins. (Without the
  // explicit check every comparison against NaN is false and the raw delta
  // logic would silently ApplyRemote.)
  EXPECT_EQ(decideSmart(0.5f, kNan, kEps), SmartAction::UploadLocal);
  EXPECT_EQ(decideSmart(0.0f, kNan, kEps), SmartAction::UploadLocal);
}

TEST(DecideSmart, InfiniteRemoteUploadsLocal) {
  EXPECT_EQ(decideSmart(0.5f, kInf, kEps), SmartAction::UploadLocal);
  EXPECT_EQ(decideSmart(0.5f, -kInf, kEps), SmartAction::UploadLocal);
}

TEST(DecideSmart, NegativeRemoteUploadsLocal) {
  // Out-of-range but finite: falls through the ordinary delta rule, and any
  // non-negative local position is ahead of it.
  EXPECT_EQ(decideSmart(0.0f, -0.5f, kEps), SmartAction::UploadLocal);
  EXPECT_EQ(decideSmart(0.3f, -0.001f, kEps), SmartAction::UploadLocal);
}

TEST(DecideSmart, NanLocalApplies) {
  // Only the remote side is guarded; a NaN local cannot be "ahead", so the
  // delta rule lands on ApplyRemote. Pinned so a change here is deliberate.
  EXPECT_EQ(decideSmart(kNan, 0.5f, kEps), SmartAction::ApplyRemote);
}

// --- preferAlternate -------------------------------------------------------

TEST(PreferAlternate, AlternateErrorKeepsPrimary) {
  EXPECT_FALSE(preferAlternate(/*primaryOk=*/true, /*primaryNotFound=*/false, 0.2f, /*altOk=*/false, 0.9f));
  EXPECT_FALSE(preferAlternate(/*primaryOk=*/false, /*primaryNotFound=*/true, 0.0f, /*altOk=*/false, 0.9f));
}

TEST(PreferAlternate, PrimaryNotFoundAlternateOkTakesAlternate) {
  EXPECT_TRUE(preferAlternate(/*primaryOk=*/false, /*primaryNotFound=*/true, 0.0f, /*altOk=*/true, 0.0f));
  EXPECT_TRUE(preferAlternate(/*primaryOk=*/false, /*primaryNotFound=*/true, 0.0f, /*altOk=*/true, 0.4f));
}

TEST(PreferAlternate, BothOkAlternateAheadTakesAlternate) {
  EXPECT_TRUE(preferAlternate(/*primaryOk=*/true, /*primaryNotFound=*/false, 0.3f, /*altOk=*/true, 0.6f));
}

TEST(PreferAlternate, BothOkPrimaryAheadKeepsPrimary) {
  EXPECT_FALSE(preferAlternate(/*primaryOk=*/true, /*primaryNotFound=*/false, 0.6f, /*altOk=*/true, 0.3f));
}

TEST(PreferAlternate, BothOkEqualKeepsPrimary) {
  // Strictly-greater: a tie keeps the configured matching method.
  EXPECT_FALSE(preferAlternate(/*primaryOk=*/true, /*primaryNotFound=*/false, 0.5f, /*altOk=*/true, 0.5f));
  EXPECT_FALSE(preferAlternate(/*primaryOk=*/true, /*primaryNotFound=*/false, 0.0f, /*altOk=*/true, 0.0f));
}

TEST(PreferAlternate, PrimaryErrorAlternateOkTakesAlternate) {
  // Primary lookup failed (network/auth/server), alternate succeeded: the
  // alternate is the only real record; the primary pct is still its initial 0.
  EXPECT_TRUE(preferAlternate(/*primaryOk=*/false, /*primaryNotFound=*/false, 0.0f, /*altOk=*/true, 0.0f));
  EXPECT_TRUE(preferAlternate(/*primaryOk=*/false, /*primaryNotFound=*/false, 0.9f, /*altOk=*/true, 0.1f));
}
