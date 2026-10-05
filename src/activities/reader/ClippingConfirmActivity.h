#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Pre-save confirmation for a highlight/clipping. Shows the whole selected text
// (wrapped and paged when it does not fit) so a mis-tapped range can be caught
// before anything is written. Confirm saves; Back/Cancel discards.
class ClippingConfirmActivity final : public Activity {
 public:
  ClippingConfirmActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string text);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  void confirm();
  void cancel();
  void nextPage(int delta);
  int linesPerPage() const;

  std::string text;
  std::vector<std::string> lines;
  int page = 0;
  int pageCount = 1;
  int fontId = 0;
  int contentX = 0;
  int contentWidth = 0;
  int titleY = 0;
  int textTop = 0;
  int textBottom = 0;
  int lineStep = 0;
  int bottomBarHeight = 0;
  bool touchDown = false;
  int touchX = 0;
  int touchY = 0;
  ButtonNavigator buttonNavigator;
};
