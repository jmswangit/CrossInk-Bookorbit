#include "ClippingConfirmActivity.h"

#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kMaxWrappedLines = 4096;
}

ClippingConfirmActivity::ClippingConfirmActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string text)
    : Activity("ClippingConfirm", renderer, mappedInput), text(std::move(text)) {}

void ClippingConfirmActivity::onEnter() {
  Activity::onEnter();
  const auto& metrics = UITheme::getInstance().getMetrics();
  fontId = UI_10_FONT_ID;
  contentX = metrics.contentSidePadding;
  contentWidth = renderer.getScreenWidth() - metrics.contentSidePadding * 2;

  const int titleHeight = renderer.getLineHeight(fontId) + 8;
  titleY = metrics.topPadding + 6;
  textTop = titleY + titleHeight + 6;
  bottomBarHeight = std::max(36, metrics.buttonHintsHeight);
  textBottom = renderer.getScreenHeight() - bottomBarHeight - 12;
  lineStep = renderer.getLineHeight(fontId) + 2;

  lines = renderer.wrappedText(fontId, text.c_str(), contentWidth, kMaxWrappedLines, EpdFontFamily::REGULAR);
  const int perPage = std::max(1, linesPerPage());
  pageCount = lines.empty() ? 1 : (static_cast<int>(lines.size()) + perPage - 1) / perPage;
  if (pageCount < 1) pageCount = 1;
  page = 0;
  requestUpdate(true);
}

int ClippingConfirmActivity::linesPerPage() const {
  if (lineStep <= 0) return 1;
  return std::max(1, (textBottom - textTop) / lineStep);
}

void ClippingConfirmActivity::nextPage(const int delta) {
  if (pageCount <= 1) return;
  page = (page + delta + pageCount) % pageCount;
  requestUpdate();
}

void ClippingConfirmActivity::confirm() {
  ActivityResult result;
  result.isCancelled = false;
  setResult(std::move(result));
  finish();
}

void ClippingConfirmActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void ClippingConfirmActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirm();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    nextPage(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    nextPage(-1);
    return;
  }

  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTouchDown(x, y)) {
    touchDown = true;
    touchX = x;
    touchY = y;
  }
  if (touchDown && mappedInput.wasScreenTouchReleased()) {
    touchDown = false;
    const int barY = renderer.getScreenHeight() - bottomBarHeight;
    if (touchY >= barY) {
      if (touchX < renderer.getScreenWidth() / 2)
        cancel();
      else
        confirm();
      return;
    }
    nextPage(1);
    return;
  }

  buttonNavigator.onNextRelease([this] { nextPage(1); });
  buttonNavigator.onPreviousRelease([this] { nextPage(-1); });
}

void ClippingConfirmActivity::render(RenderLock&&) {
  const bool black = ReaderUtils::readerForegroundBlack();
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  renderer.drawText(fontId, contentX, titleY, tr(STR_SAVE_HIGHLIGHT), black, EpdFontFamily::BOLD);

  const int perPage = std::max(1, linesPerPage());
  const int start = page * perPage;
  int y = textTop;
  for (int i = 0; i < perPage && start + i < static_cast<int>(lines.size()); ++i) {
    renderer.drawText(fontId, contentX, y, lines[start + i].c_str(), black, EpdFontFamily::REGULAR);
    y += lineStep;
  }

  if (pageCount > 1) {
    char pageBuf[32];
    snprintf(pageBuf, sizeof(pageBuf), tr(STR_SYNC_PAGE_TOTAL_FORMAT), page + 1, pageCount);
    const int pageWidth = renderer.getTextWidth(SMALL_FONT_ID, pageBuf, EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, (renderer.getScreenWidth() - pageWidth) / 2, textBottom, pageBuf, black,
                      EpdFontFamily::REGULAR);
  }

  // Touch buttons: Cancel (left) / Save (right).
  const int barY = renderer.getScreenHeight() - bottomBarHeight;
  const int half = renderer.getScreenWidth() / 2;
  renderer.drawRect(1, barY + 1, half - 2, bottomBarHeight - 2, black);
  renderer.drawRect(half + 1, barY + 1, half - 2, bottomBarHeight - 2, black);
  const char* cancelLabel = tr(STR_CANCEL);
  const char* saveLabel = tr(STR_CONFIRM);
  const int cancelW = renderer.getTextWidth(fontId, cancelLabel, EpdFontFamily::REGULAR);
  const int saveW = renderer.getTextWidth(fontId, saveLabel, EpdFontFamily::REGULAR);
  const int labelY = barY + (bottomBarHeight - renderer.getLineHeight(fontId)) / 2;
  renderer.drawText(fontId, (half - cancelW) / 2, labelY, cancelLabel, black, EpdFontFamily::REGULAR);
  renderer.drawText(fontId, half + (half - saveW) / 2, labelY, saveLabel, black, EpdFontFamily::REGULAR);

  renderer.displayBuffer();
}
