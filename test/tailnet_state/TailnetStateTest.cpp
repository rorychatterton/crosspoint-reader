#include <gtest/gtest.h>

#include <cstdlib>

#include "microlink.h"
#include "ml_memory_budget.h"
#include "ml_startup_policy.h"
#include "ml_workspace.h"

namespace {
struct HeapModel {
  size_t freeBytes;
  size_t largestBlock;
  unsigned allocations = 0;
  unsigned frees = 0;
};

void* modelAllocate(size_t size, void* context) {
  auto& heap = *static_cast<HeapModel*>(context);
  ++heap.allocations;
  if (size > heap.freeBytes || size > heap.largestBlock) return nullptr;
  void* result = std::malloc(size);
  if (result) heap.freeBytes -= size;
  return result;
}

void modelFree(void* ptr, void* context) {
  auto& heap = *static_cast<HeapModel*>(context);
  ++heap.frees;
  std::free(ptr);
}
}  // namespace

TEST(TailnetState, EveryDeclaredStateHasDiagnosticText) {
  EXPECT_STREQ(microlink_state_name(ML_STATE_IDLE), "idle");
  EXPECT_STREQ(microlink_state_name(ML_STATE_WIFI_WAIT), "waiting for Wi-Fi");
  EXPECT_STREQ(microlink_state_name(ML_STATE_CONNECTING), "connecting to control plane");
  EXPECT_STREQ(microlink_state_name(ML_STATE_REGISTERING), "registering/fetching peer map");
  EXPECT_STREQ(microlink_state_name(ML_STATE_CONNECTED), "connected");
  EXPECT_STREQ(microlink_state_name(ML_STATE_RECONNECTING), "reconnecting to control plane");
  EXPECT_STREQ(microlink_state_name(ML_STATE_ERROR), "client error");
}

TEST(TailnetState, InvalidValueIsNotReportedAsUnknown) {
  EXPECT_STREQ(microlink_state_name(static_cast<microlink_state_t>(255)), "invalid state");
}

TEST(TailnetState, OnlyErrorStateIsTerminal) {
  for (int value = ML_STATE_IDLE; value <= ML_STATE_RECONNECTING; ++value) {
    EXPECT_FALSE(microlink_state_is_terminal_error(static_cast<microlink_state_t>(value)));
  }
  EXPECT_TRUE(microlink_state_is_terminal_error(ML_STATE_ERROR));
  EXPECT_FALSE(microlink_state_is_terminal_error(static_cast<microlink_state_t>(255)));
}

TEST(TailnetState, C3NoiseBuffersStayWithinTheirHeapBudgets) {
  EXPECT_EQ(ML_TARGET_MAP_FRAME_STORAGE_SIZE, 16416u);
  EXPECT_LE(ML_TARGET_MAP_FRAME_STORAGE_SIZE, 17u * 1024u);
}

TEST(TailnetState, TargetedBootstrapDefersDataPlaneStacks) {
  EXPECT_TRUE(ml_has_target(0x64400001u, nullptr));
  EXPECT_TRUE(ml_has_target(0, "peer.tailnet.ts.net"));
  EXPECT_FALSE(ml_has_target(0, nullptr));
  EXPECT_FALSE(ml_has_target(0, ""));

  EXPECT_EQ(ML_DATA_PLANE_STACK_BYTES, 12u * 1024u);
  EXPECT_EQ(ML_TARGET_BOOTSTRAP_RESERVED_BYTES,
            ML_TASK_COORD_STACK_SIZE + ML_TARGET_MAP_FRAME_STORAGE_SIZE);
  EXPECT_LT(ML_TARGET_BOOTSTRAP_RESERVED_BYTES,
            ML_TASK_COORD_STACK_SIZE + ML_DATA_PLANE_STACK_BYTES +
                ML_TARGET_MAP_FRAME_STORAGE_SIZE);
}

TEST(TailnetState, EarlyWorkspaceSurvivesLaterFragmentation) {
  HeapModel heap{.freeBytes = 23576, .largestBlock = 20468};
  ml_workspace_t workspace{};
  ASSERT_TRUE(ml_workspace_reserve(&workspace, ML_TARGET_MAP_FRAME_STORAGE_SIZE,
                                   modelAllocate, modelFree, &heap));
  uint8_t* reserved = ml_workspace_get(&workspace, ML_TARGET_MAP_FRAME_STORAGE_SIZE);
  ASSERT_NE(reserved, nullptr);

  heap.largestBlock = 4096;
  EXPECT_EQ(ml_workspace_get(&workspace, ML_TARGET_MAP_FRAME_STORAGE_SIZE), reserved);
  EXPECT_EQ(heap.allocations, 1u);

  ml_workspace_release(&workspace);
  EXPECT_EQ(heap.frees, 1u);
  EXPECT_EQ(ml_workspace_get(&workspace, 1), nullptr);
}

TEST(TailnetState, FragmentedHeapRejectsLateWorkspaceWithoutPartialState) {
  HeapModel heap{.freeBytes = 23576, .largestBlock = 8192};
  ml_workspace_t workspace{};
  EXPECT_FALSE(ml_workspace_reserve(&workspace, ML_TARGET_MAP_FRAME_STORAGE_SIZE,
                                    modelAllocate, modelFree, &heap));
  EXPECT_EQ(workspace.data, nullptr);
  EXPECT_EQ(workspace.capacity, 0u);
  EXPECT_EQ(heap.allocations, 1u);
  EXPECT_EQ(heap.frees, 0u);
}

TEST(TailnetState, WorkspaceRejectsOversizedViewsAndDoubleReservation) {
  HeapModel heap{.freeBytes = 32768, .largestBlock = 32768};
  ml_workspace_t workspace{};
  ASSERT_TRUE(ml_workspace_reserve(&workspace, ML_TARGET_MAP_FRAME_STORAGE_SIZE,
                                   modelAllocate, modelFree, &heap));
  EXPECT_EQ(ml_workspace_get(&workspace, ML_TARGET_MAP_FRAME_STORAGE_SIZE + 1), nullptr);
  EXPECT_FALSE(ml_workspace_reserve(&workspace, 1024, modelAllocate, modelFree, &heap));
  EXPECT_EQ(heap.allocations, 1u);
  ml_workspace_release(&workspace);
  ml_workspace_release(&workspace);
  EXPECT_EQ(heap.frees, 1u);
}
