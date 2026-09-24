#include "LibraryActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/listIcons.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr char kFilterPath[] = "/.crosspoint/library.filter";
constexpr unsigned long LONG_PRESS_MS = 1000;

// Nudge the header search icon down so it sits optically centered in the band.
constexpr int kSearchIconYOffset = 8;
constexpr int kCoverAspectW = 123;
constexpr int kCoverAspectH = 180;
constexpr int kCoverCornerRadius = 2;
constexpr int kTitleStripHeight = 32;
constexpr int kSelectionPadding = 4;
constexpr int kSelectionOutlineGap = 2;
constexpr int kSelectionOuterInset = kSelectionPadding + kSelectionOutlineGap;

const char* tabLabel(const int index) {
  switch (index) {
    case 0:
      return tr(STR_RECENT_TAB);
    case 1:
      return tr(STR_SORT_TITLE);
    case 2:
      return tr(STR_SORT_AUTHOR);
    default:
      return tr(STR_SORT_SERIES);
  }
}

void calculateCoverFillCrop(const Bitmap& bitmap, float& cropX, float& cropY, const int targetW, const int targetH) {
  cropX = 0.0f;
  cropY = 0.0f;
  const float srcW = static_cast<float>(bitmap.getWidth());
  const float srcH = static_cast<float>(bitmap.getHeight());
  if (srcW <= 0.0f || srcH <= 0.0f) return;
  const float srcRatio = srcW / srcH;
  const float targetRatio = static_cast<float>(targetW) / static_cast<float>(targetH);
  if (srcRatio > targetRatio) {
    cropX = std::max(0.0f, 1.0f - (targetRatio / srcRatio));
  } else if (srcRatio < targetRatio) {
    cropY = std::max(0.0f, 1.0f - (srcRatio / targetRatio));
  }
}

std::string coverThumbPathFor(const std::string& path, const int width, const int height) {
  if (FsHelpers::hasEpubExtension(path)) {
    return UITheme::getCoverThumbPath(Epub(path, "/.crosspoint").getThumbBmpPath(), width, height);
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return UITheme::getCoverThumbPath(Xtc(path, "/.crosspoint").getThumbBmpPath(), width, height);
  }
  return {};
}
}  // namespace

LibraryActivity::LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Library", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void LibraryActivity::ensureIndex() {
  if (indexReady) return;
  if (!library::isLibraryIndexDirty() && index.open(library::libraryIndexPath())) {
    // The index can be stale because files were added by a source the activity
    // hooks did not see (for example a local-network download). Compare the
    // on-card book count and rebuild only when it actually changed.
    const uint16_t current = library::countCardBooks("/");
    if (current == index.bookCount()) {
      indexReady = true;
      return;
    }
    LOG_INF("LIBUI", "book count changed %u -> %u; rebuilding", static_cast<unsigned>(index.bookCount()),
            static_cast<unsigned>(current));
    index.close();
  }
  GUI.drawPopup(renderer, tr(STR_LIBRARY_BUILDING));
  renderer.displayBuffer();
  library::BuildStats stats{};
  const bool built = library::buildLibraryIndex("/", stats, /*readMetadata=*/true);
  if (built && index.open(library::libraryIndexPath())) {
    indexReady = true;
    LOG_INF("LIBUI", "library ready: %u books (%u parsed, %u reused)", static_cast<unsigned>(index.bookCount()),
            static_cast<unsigned>(stats.parsed), static_cast<unsigned>(stats.metadataReused));
    // Only after a rebuild: fill in any covers the card is still missing, with
    // the progress popup. On a plain reopen there is nothing new to do.
    prefetchMissingCovers();
  } else {
    buildFailed = true;
    LOG_ERR("LIBUI", "library index unavailable");
  }
}

size_t LibraryActivity::rowCount() const { return mode == Mode::Groups ? groups.size() : items.size(); }

bool LibraryActivity::rowOrdinal(const size_t row, uint16_t& ordinal) {
  if (!indexReady || row >= items.size()) return false;
  ordinal = items[row];
  return true;
}

void LibraryActivity::buildSearch() {
  items.clear();
  if (!indexReady || query.empty()) return;
  const std::string needle = library::fold(query);
  if (needle.empty()) return;

  const uint16_t n = index.bookCount();
  items.reserve(n);
  for (uint16_t row = 0; row < n; row++) {
    const uint16_t ordinal = index.ordinalForRow(library::SortOrder::TitleAsc, row);
    if (ordinal == 0xFFFF) continue;
    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record)) continue;
    bool match = library::matchesQuery(std::string_view(record.fold, record.foldLen), needle);
    if (!match) {
      std::string author;
      if (index.readAuthor(record, author) && !author.empty()) {
        const std::string folded = library::fold(author);
        match = library::matchesQuery(folded, needle);
      }
    }
    if (!match) {
      std::string series;
      if (index.readSeries(record, series)) {
        const std::string folded = library::fold(series);
        match = library::matchesQuery(folded, needle);
      }
    }
    if (match && !passesFilter(record)) match = false;
    if (match) items.push_back(ordinal);
  }
}

bool LibraryActivity::passesFilter(const library::ClixRecord& record) const {
  const bool epubOnly =
      filter == Filter::EpubAll || filter == Filter::EpubUnread || filter == Filter::EpubFinished;
  const bool wantUnread = filter == Filter::Unread || filter == Filter::EpubUnread;
  const bool wantFinished = filter == Filter::Finished || filter == Filter::EpubFinished;
  if (epubOnly && (record.flags & library::CLIX_BOOK_FLAG_EPUB) == 0) return false;
  if (wantUnread || wantFinished) {
    const bool completed = library::LibraryIndexFile::isCompleted(record);
    if (wantUnread && completed) return false;
    if (wantFinished && !completed) return false;
  }
  return true;
}

void LibraryActivity::openFilterMenu() {
  std::vector<std::string> options = {tr(STR_FILTER_ALL),         tr(STR_FILTER_UNREAD),
                                      tr(STR_FILTER_FINISHED),    tr(STR_FILTER_EPUB_ALL),
                                      tr(STR_FILTER_EPUB_UNREAD), tr(STR_FILTER_EPUB_FINISHED)};
  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "LibraryFilter", StrId::STR_FILTER, options,
                                                static_cast<uint8_t>(filter)),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto* selection = std::get_if<OptionSelectionResult>(&result.data);
          if (selection != nullptr && selection->index < 6) {
            filter = static_cast<Filter>(selection->index);
            saveFilter();
            rebuildContent();
          }
        }
        requestUpdate();
      });
}

void LibraryActivity::loadFilter() {
  filter = Filter::All;
  FsFile f;
  if (!Storage.openFileForRead("LIBUI", kFilterPath, f)) return;
  uint8_t v = 0;
  if (f.read(&v, 1) == 1 && v <= static_cast<uint8_t>(Filter::EpubFinished)) filter = static_cast<Filter>(v);
  f.close();
}

void LibraryActivity::saveFilter() const {
  FsFile f;
  if (!Storage.openFileForWrite("LIBUI", kFilterPath, f)) return;
  const uint8_t v = static_cast<uint8_t>(filter);
  f.write(&v, 1);
  f.close();
}

void LibraryActivity::buildGroups(const bool byAuthor) {
  groups.clear();
  if (!indexReady) return;
  const uint16_t n = index.bookCount();
  const library::SortOrder order = byAuthor ? library::SortOrder::AuthorAsc : library::SortOrder::SeriesAsc;

  uint16_t row = 0;
  while (row < n) {
    const uint16_t ordinal = index.ordinalForRow(order, row);
    if (ordinal == 0xFFFF) {
      row++;
      continue;
    }
    library::ClixRecord first{};
    if (!index.readRecord(ordinal, first)) {
      row++;
      continue;
    }
    std::string name;
    if (byAuthor) {
      index.readAuthor(first, name);
      if (name.empty()) name = tr(STR_UNKNOWN_AUTHOR);
    } else {
      if (!index.readSeries(first, name)) name = tr(STR_NO_SERIES);
    }

    const uint16_t start = row;
    row++;
    while (row < n) {
      const uint16_t nextOrdinal = index.ordinalForRow(order, row);
      if (nextOrdinal == 0xFFFF) break;
      library::ClixRecord next{};
      if (!index.readRecord(nextOrdinal, next)) break;
      const bool same = byAuthor
                            ? (next.authorKeyLen == first.authorKeyLen &&
                               memcmp(next.authorKey, first.authorKey, first.authorKeyLen) == 0)
                            : (memcmp(next.seriesKey, first.seriesKey, library::CLIX_SERIES_FOLD_BYTES) == 0);
      if (!same) break;
      row++;
    }
    groups.push_back(Group{name, start, static_cast<uint16_t>(row - start)});
  }
  if (reversed) std::reverse(groups.begin(), groups.end());
}

void LibraryActivity::rebuildContent() {
  items.clear();
  groups.clear();
  drilled = false;
  groupName.clear();
  selectorIndex = 0;
  topIndex = 0;
  listTop = 0;
  loadedPageStart = kNoPageLoaded;
  listNav.reset(0);
  if (!indexReady) return;

  // A search is a global book lookup (title, author or series) shown as a list,
  // available from every tab.
  if (!query.empty()) {
    mode = Mode::List;
    buildSearch();
    return;
  }

  switch (tab) {
    case Tab::Recent: {
      mode = Mode::Grid;
      const auto order = reversed ? library::SortOrder::RecentAsc : library::SortOrder::RecentDesc;
      const uint16_t n = index.bookCount();
      items.reserve(n);
      for (uint16_t row = 0; row < n; row++) {
        const uint16_t ordinal = index.ordinalForRow(order, row);
        if (ordinal == 0xFFFF) continue;
        if (filter != Filter::All) {
          library::ClixRecord record{};
          if (!index.readRecord(ordinal, record) || !passesFilter(record)) continue;
        }
        items.push_back(ordinal);
      }
      break;
    }
    case Tab::Title: {
      mode = Mode::Grid;
      const auto order = reversed ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
      const uint16_t n = index.bookCount();
      items.reserve(n);
      for (uint16_t row = 0; row < n; row++) {
        const uint16_t ordinal = index.ordinalForRow(order, row);
        if (ordinal == 0xFFFF) continue;
        if (filter != Filter::All) {
          library::ClixRecord record{};
          if (!index.readRecord(ordinal, record) || !passesFilter(record)) continue;
        }
        items.push_back(ordinal);
      }
      break;
    }
    case Tab::Author:
      mode = Mode::Groups;
      buildGroups(true);
      break;
    case Tab::Series:
      mode = Mode::Groups;
      buildGroups(false);
      break;
  }
}

void LibraryActivity::openGroup(const size_t groupIndex) {
  if (groupIndex >= groups.size()) return;
  const Group& group = groups[groupIndex];
  groupName = group.name;
  drilled = true;
  // Both an author and a series open as a 2x2 cover grid.
  mode = Mode::Grid;
  items.clear();
  const auto order = tab == Tab::Author ? library::SortOrder::AuthorAsc : library::SortOrder::SeriesAsc;
  items.reserve(group.count);
  for (uint16_t i = 0; i < group.count; i++) {
    const uint16_t ordinal = index.ordinalForRow(order, static_cast<uint16_t>(group.firstRow + i));
    if (ordinal == 0xFFFF) continue;
    if (filter != Filter::All) {
      library::ClixRecord record{};
      if (!index.readRecord(ordinal, record) || !passesFilter(record)) continue;
    }
    items.push_back(ordinal);
  }
  selectorIndex = 0;
  topIndex = 0;
  listNav.reset(0);
  requestUpdate();
}

void LibraryActivity::switchTab(const int tabIndex) {
  const int clamped = std::clamp(tabIndex, 0, kTabCount - 1);
  if (clamped == static_cast<int>(tab) && query.empty() && !drilled) return;
  tab = static_cast<Tab>(clamped);
  reversed = false;
  query.clear();
  rebuildContent();
  requestUpdate();
}

void LibraryActivity::toggleReverse() {
  reversed = !reversed;
  rebuildContent();
  requestUpdate();
}

void LibraryActivity::openSearch() {
  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH), query);
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      query = std::get<KeyboardResult>(result.data).text;
      rebuildContent();
    }
    requestUpdate();
  });
}

void LibraryActivity::moveSelection(const int index) {
  const int count = static_cast<int>(rowCount());
  if (count == 0) return;
  selectorIndex = static_cast<size_t>(std::clamp(index, 0, count - 1));
  listNav.follow(count);
  topIndex = listNav.top;
  requestUpdate();
}

void LibraryActivity::activateSelected() {
  if (mode == Mode::Groups) {
    openGroup(selectorIndex);
    return;
  }
  uint16_t ordinal = 0xFFFF;
  if (!rowOrdinal(selectorIndex, ordinal)) return;
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record)) return;
  std::string path;
  if (!index.readPath(record, path)) return;
  index.close();
  indexReady = false;
  onSelectBook(path);
}

void LibraryActivity::goUp() {
  if (!query.empty()) {
    query.clear();
    rebuildContent();
    requestUpdate();
    return;
  }
  if (drilled) {
    rebuildContent();
    requestUpdate();
    return;
  }
  onGoHome();
}

void LibraryActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LibraryActivity*>(user);
  if (event.value < 0) return;
  const size_t row = static_cast<size_t>(event.value);
  if (row >= self->rowCount()) return;
  self->selectorIndex = row;
  self->app.clearTapFlash();
  self->activateSelected();
}

void LibraryActivity::onEnter() {
  Activity::onEnter();
  selectorIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  uiReady = false;
  backLongPressFired = false;
  confirmLongPressFired = false;
  tab = Tab::Recent;
  reversed = false;
  query.clear();
  loadFilter();

  ensureIndex();
  rebuildContent();

  listNav.top = 0;
  listNav.visibleRows = visibleRows;

  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &LibraryActivity::onRowEvent, this);
  app.setScreen(&LibraryActivity::listScreen, this);
  requestUpdate();
}

void LibraryActivity::onExit() {
  Activity::onExit();
  index.close();
  indexReady = false;
  buildFailed = false;
  query.clear();
  items.clear();
  groups.clear();
}

Rect LibraryActivity::tabBandRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput);
  const int height = renderer.getLineHeight(UI_10_FONT_ID) + 12;
  return Rect{0, top, renderer.getScreenWidth(), height};
}

Rect LibraryActivity::contentRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect band = tabBandRect();
  const int statusH = renderer.getLineHeight(SMALL_FONT_ID) + 8;
  const int top = band.y + band.height + 4;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - statusH;
  return Rect{metrics.contentSidePadding, top, renderer.getScreenWidth() - metrics.contentSidePadding * 2,
              bottom - top};
}

Rect LibraryActivity::filterIconRect() const {
  const Rect search = searchIconRect();
  constexpr int kGap = 6;
  return Rect{search.x - search.width - kGap, search.y, search.width, search.height};
}

Rect LibraryActivity::searchIconRect() const {
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const auto backLayout = TouchHeaderBackButton::layout(header);
  const int w = backLayout.iconRect.width;
  const int h = backLayout.iconRect.height;
  const int y = backLayout.iconRect.y + kSearchIconYOffset;
  return Rect{header.x + header.width - w, y, w, h};
}

int LibraryActivity::tabIndexFromPoint(const int x, const int y) const {
  const Rect band = tabBandRect();
  if (y < band.y || y >= band.y + band.height) return -1;
  const int colW = renderer.getScreenWidth() / kTabCount;
  const int index = x / std::max(1, colW);
  return index >= 0 && index < kTabCount ? index : -1;
}

void LibraryActivity::coverSizeFor(const int cols, const int rows, int& outW, int& outH) const {
  const Rect area = contentRect();
  const int gap = UITheme::getInstance().getMetrics().verticalSpacing;
  const int rowSpacing = gap + 4;
  int cw = std::max(1, (area.width - (cols - 1) * gap) / cols);
  int ch = cw * kCoverAspectH / kCoverAspectW;
  const int availH = std::max(1, area.height - kTitleStripHeight);
  const int maxCh = (availH - (rows - 1) * rowSpacing) / rows;
  if (maxCh > 0 && ch > maxCh) {
    ch = maxCh;
    cw = std::max(1, ch * kCoverAspectW / kCoverAspectH);
  }
  outW = cw;
  outH = ch;
}

void LibraryActivity::updateGridGeometry() {
  // Author/series drill-downs are 2x2; Recent and Title are 3x3.
  gridCols = drilled ? 2 : 3;
  gridRows = drilled ? 2 : 3;
  coverSizeFor(gridCols, gridRows, coverWidth, coverHeight);
}

int LibraryActivity::gridIndexFromPoint(const int x, const int y) {
  updateGridGeometry();
  const Rect area = contentRect();
  const int gap = UITheme::getInstance().getMetrics().verticalSpacing;
  const int rowSpacing = gap + 4;
  const int gridTop = area.y + kTitleStripHeight;
  const int gridW = gridCols * coverWidth + (gridCols - 1) * gap;
  const int startX = area.x + (area.width - gridW) / 2;
  const int perPage = booksPerPage();
  const int pageStart = (static_cast<int>(selectorIndex) / perPage) * perPage;
  const int pageCount = std::min(perPage, static_cast<int>(items.size()) - pageStart);
  for (int i = 0; i < pageCount; i++) {
    const int col = i % gridCols;
    const int row = i / gridCols;
    const int cx = startX + col * (coverWidth + gap);
    const int cy = gridTop + row * (coverHeight + rowSpacing);
    if (x >= cx - kSelectionOuterInset && x < cx + coverWidth + kSelectionOuterInset && y >= cy - kSelectionOuterInset &&
        y < cy + coverHeight + kSelectionOuterInset) {
      return pageStart + i;
    }
  }
  return -1;
}

void LibraryActivity::drawTabs() const {
  const Rect band = tabBandRect();
  const int colW = renderer.getScreenWidth() / kTabCount;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  for (int i = 0; i < kTabCount; i++) {
    const char* label = tabLabel(i);
    const bool selected = static_cast<int>(tab) == i;
    const auto style = selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, label, style);
    const int tx = i * colW + (colW - textW) / 2;
    const int ty = band.y + (band.height - lineH) / 2;
    renderer.drawText(UI_10_FONT_ID, tx, ty, label, true, style);
    if (selected) {
      renderer.fillRect(tx, band.y + band.height - 3, textW, 3, true);
      // Sort-direction chevron beside the selected tab; tapping the selected
      // tab reverses the order.
      const int ax = tx + textW + 5;
      const int ay = band.y + (band.height - 4) / 2;
      for (int r = 0; r < 4; ++r) {
        renderer.fillRect(ax + r, reversed ? ay + (3 - r) : ay + r, 7 - 2 * r, 1, true);
      }
    }
  }
}

void LibraryActivity::drawGrid() {
  updateGridGeometry();
  const Rect area = contentRect();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int gap = metrics.verticalSpacing;
  const int rowSpacing = gap + 4;
  const int gridTop = area.y + kTitleStripHeight;
  const int gridW = gridCols * coverWidth + (gridCols - 1) * gap;
  const int startX = area.x + (area.width - gridW) / 2;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int perPage = booksPerPage();

  if (items.empty()) {
    renderer.drawText(UI_10_FONT_ID, area.x, area.y + 20, tr(STR_LIBRARY_EMPTY));
    return;
  }

  if (selectorIndex < items.size()) {
    uint16_t ordinal = 0xFFFF;
    std::string title;
    if (rowOrdinal(selectorIndex, ordinal)) {
      library::ClixRecord record{};
      if (index.readRecord(ordinal, record)) {
        std::string name;
        index.readName(record, name);
        if (!index.readTitle(record, title)) title = name;
      }
    }
    const std::string truncated = renderer.truncatedText(UI_10_FONT_ID, title.c_str(), area.width, EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, area.x, area.y + (kTitleStripHeight - lineH) / 2, truncated.c_str(), true,
                      EpdFontFamily::REGULAR);
  }

  const int pageStart = (static_cast<int>(selectorIndex) / perPage) * perPage;
  const int pageCount = std::min(perPage, static_cast<int>(items.size()) - pageStart);
  for (int i = 0; i < pageCount; i++) {
    uint16_t ordinal = 0xFFFF;
    if (!rowOrdinal(static_cast<size_t>(pageStart + i), ordinal)) continue;
    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record)) continue;
    std::string path;
    if (!index.readPath(record, path)) continue;

    const int col = i % gridCols;
    const int row = i / gridCols;
    const int bx = startX + col * (coverWidth + gap);
    const int by = gridTop + row * (coverHeight + rowSpacing);

    bool drawn = false;
    const std::string thumb = coverThumbPathFor(path, coverWidth, coverHeight);
    if (!thumb.empty() && Storage.exists(thumb.c_str())) {
      FsFile file;
      if (Storage.openFileForRead("LIBUI", thumb, file)) {
        Bitmap bmp(file);
        if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
          float cropX = 0.0f;
          float cropY = 0.0f;
          calculateCoverFillCrop(bmp, cropX, cropY, coverWidth, coverHeight);
          renderer.fillRoundedRect(bx, by, coverWidth, coverHeight, kCoverCornerRadius, Color::White);
          renderer.drawBitmap(bmp, bx, by, coverWidth, coverHeight, cropX, cropY);
          renderer.maskRoundedRectOutsideCorners(bx, by, coverWidth, coverHeight, kCoverCornerRadius, Color::White);
          renderer.drawRoundedRect(bx, by, coverWidth, coverHeight, 2, kCoverCornerRadius, true);
          drawn = true;
        }
        file.close();
      }
    }
    if (!drawn) {
      renderer.fillRoundedRect(bx, by, coverWidth, coverHeight, kCoverCornerRadius, Color::White);
      renderer.drawRoundedRect(bx, by, coverWidth, coverHeight, 2, kCoverCornerRadius, true);
      drawLucideIcon(renderer, icon_book_marked_32, bx + (coverWidth - 32) / 2, by + (coverHeight - 32) / 2);
    }
    if (library::LibraryIndexFile::isCompleted(record)) {
      // Finished badge in the cover's top-right corner.
      constexpr int kBadge = 24;
      const int bbx = bx + coverWidth - kBadge - 2;
      const int bby = by + 2;
      renderer.fillRoundedRect(bbx, bby, kBadge, kBadge, 4, Color::White);
      renderer.drawRoundedRect(bbx, bby, kBadge, kBadge, 1, 4, true);
      drawLucideIcon(renderer, icon_check_24, bbx, bby);
    }
    if (pageStart + i == static_cast<int>(selectorIndex)) {
      renderer.drawRoundedRect(bx - kSelectionPadding, by - kSelectionPadding, coverWidth + kSelectionPadding * 2,
                               coverHeight + kSelectionPadding * 2, 3, kCoverCornerRadius + kSelectionPadding, true);
      renderer.drawRoundedRect(bx - kSelectionOuterInset, by - kSelectionOuterInset,
                               coverWidth + kSelectionOuterInset * 2, coverHeight + kSelectionOuterInset * 2, 1,
                               kCoverCornerRadius + kSelectionOuterInset, true);
    }
  }
}

void LibraryActivity::ensurePageCovers() {
  if (mode != Mode::Grid || !indexReady || items.empty()) return;
  updateGridGeometry();
  // The two grids the app can show, so a cover is generated at both sizes the
  // first time it is cached (3x3 for Recent/Title, 2x2 for a drill-down).
  int w3, h3, w2, h2;
  coverSizeFor(3, 3, w3, h3);
  coverSizeFor(2, 2, w2, h2);
  const int perPage = booksPerPage();
  const int total = static_cast<int>(items.size());
  const int pageStart = (static_cast<int>(selectorIndex) / perPage) * perPage;
  if (loadedPageStart == pageStart) return;
  const int pageEnd = std::min(pageStart + perPage, total);

  std::vector<std::string> paths;
  paths.reserve(pageEnd - pageStart);
  bool needsGeneration = false;
  for (int i = pageStart; i < pageEnd; i++) {
    uint16_t ordinal = 0xFFFF;
    if (!rowOrdinal(static_cast<size_t>(i), ordinal)) continue;
    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record)) continue;
    std::string path;
    if (!index.readPath(record, path)) continue;
    if (FsHelpers::hasEpubExtension(path)) {
      const std::string t3 = coverThumbPathFor(path, w3, h3);
      const std::string t2 = coverThumbPathFor(path, w2, h2);
      if ((!t3.empty() && !Storage.exists(t3.c_str())) || (!t2.empty() && !Storage.exists(t2.c_str())))
        needsGeneration = true;
    }
    paths.push_back(std::move(path));
  }
  if (!needsGeneration) {
    loadedPageStart = pageStart;
    return;
  }

  index.close();
  bool showing = false;
  Rect popup{};
  const int totalToProcess = std::max(1, static_cast<int>(paths.size()));
  int processed = 0;
  for (const auto& path : paths) {
    if (FsHelpers::hasEpubExtension(path)) {
      const std::string t3 = coverThumbPathFor(path, w3, h3);
      const std::string t2 = coverThumbPathFor(path, w2, h2);
      const bool missing3 = !t3.empty() && !Storage.exists(t3.c_str());
      const bool missing2 = !t2.empty() && !Storage.exists(t2.c_str());
      if (missing3 || missing2) {
        Epub epub(path, "/.crosspoint");
        if (epub.load(true, true, Epub::XLocationLoadMode::Skip)) {
          if (!showing) {
            showing = true;
            popup = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popup, 10 + (processed * 90) / totalToProcess);
          if (missing3) epub.generateThumbBmp(w3, h3, &renderer, SETTINGS.getReaderFontId());
          if (missing2) epub.generateThumbBmp(w2, h2, &renderer, SETTINGS.getReaderFontId());
        }
      }
    }
    processed++;
  }

  if (!index.open(library::libraryIndexPath())) {
    indexReady = false;
    buildFailed = true;
  }
  loadedPageStart = pageStart;
  if (showing) requestUpdate();
}

void LibraryActivity::prefetchMissingCovers() {
  if (!indexReady) return;
  int w3, h3, w2, h2;
  coverSizeFor(3, 3, w3, h3);
  coverSizeFor(2, 2, w2, h2);

  // Collect paths first: generating a cover opens the book, and only one reader
  // can hold the card at a time.
  std::vector<std::string> paths;
  const uint16_t n = index.bookCount();
  for (uint16_t ordinal = 0; ordinal < n; ordinal++) {
    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record)) continue;
    std::string path;
    if (!index.readPath(record, path)) continue;
    if (!FsHelpers::hasEpubExtension(path)) continue;
    const std::string t3 = coverThumbPathFor(path, w3, h3);
    const std::string t2 = coverThumbPathFor(path, w2, h2);
    if ((!t3.empty() && !Storage.exists(t3.c_str())) || (!t2.empty() && !Storage.exists(t2.c_str())))
      paths.push_back(std::move(path));
  }
  if (paths.empty()) return;

  index.close();
  Rect popup = GUI.drawPopup(renderer, tr(STR_LIBRARY_COVERS));
  renderer.displayBuffer();
  const int totalToProcess = std::max(1, static_cast<int>(paths.size()));
  int processed = 0;
  for (const auto& path : paths) {
    const std::string t3 = coverThumbPathFor(path, w3, h3);
    const std::string t2 = coverThumbPathFor(path, w2, h2);
    const bool missing3 = !t3.empty() && !Storage.exists(t3.c_str());
    const bool missing2 = !t2.empty() && !Storage.exists(t2.c_str());
    if (missing3 || missing2) {
      Epub epub(path, "/.crosspoint");
      if (epub.load(true, true, Epub::XLocationLoadMode::Skip)) {
        if (missing3) epub.generateThumbBmp(w3, h3, &renderer, SETTINGS.getReaderFontId());
        if (missing2) epub.generateThumbBmp(w2, h2, &renderer, SETTINGS.getReaderFontId());
      }
    }
    processed++;
    if ((processed & 0x3) == 0) {
      GUI.fillPopupProgress(renderer, popup, 100 * processed / totalToProcess);
      renderer.displayBuffer();
    }
  }
  if (!index.open(library::libraryIndexPath())) {
    indexReady = false;
    buildFailed = true;
  }
}

void LibraryActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, TouchHeaderBackButton::headerRect(renderer, mappedInput))) {
    onGoHome();
    return;
  }

  if (!backLongPressFired && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    backLongPressFired = true;
    toggleReverse();
    return;
  }
  if (backLongPressFired) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) backLongPressFired = false;
    return;
  }

  if (!confirmLongPressFired && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    confirmLongPressFired = true;
    openSearch();
    return;
  }
  if (confirmLongPressFired) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) confirmLongPressFired = false;
    return;
  }

  // Touch swipes: horizontal switches tabs, vertical pages the grid or scrolls
  // a list. Handled before tap routing so a swipe is never also a tap.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe != MappedInputManager::SwipeDir::None) {
    if (swipe == MappedInputManager::SwipeDir::Left) {
      switchTab(static_cast<int>(tab) + 1);
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Right) {
      switchTab(static_cast<int>(tab) - 1);
      return;
    }
    const int count = static_cast<int>(rowCount());
    if (mode == Mode::Grid) {
      if (count > 0) {
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? booksPerPage() : -booksPerPage();
        selectorIndex = static_cast<size_t>(std::clamp(static_cast<int>(selectorIndex) + delta, 0, count - 1));
        requestUpdate();
      }
      return;
    }
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    if (listNav.scrollBy(delta, count)) {
      topIndex = listNav.top;
      requestUpdate();
    }
    return;
  }

  if (uiReady || mode == Mode::Grid) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchReleased && snap.touchX >= 0) {
      const int tabIndex = tabIndexFromPoint(snap.touchX, snap.touchY);
      if (tabIndex >= 0) {
        // Tapping the already-selected tab reverses the sort; another tab
        // switches to it.
        if (tabIndex == static_cast<int>(tab))
          toggleReverse();
        else
          switchTab(tabIndex);
        return;
      }
      // The header search icon is only an app action in list mode, so handle it
      // directly here to keep it working on the grid tab too.
      if (mappedInput.hasTouchHardware()) {
        const Rect search = searchIconRect();
        if (snap.touchX >= search.x && snap.touchX < search.x + search.width && snap.touchY >= search.y &&
            snap.touchY < search.y + search.height) {
          openSearch();
          return;
        }
        const Rect filterRect = filterIconRect();
        if (snap.touchX >= filterRect.x && snap.touchX < filterRect.x + filterRect.width &&
            snap.touchY >= filterRect.y && snap.touchY < filterRect.y + filterRect.height) {
          openFilterMenu();
          return;
        }
      }
      if (mode == Mode::Grid) {
        const int hit = gridIndexFromPoint(snap.touchX, snap.touchY);
        if (hit >= 0) {
          selectorIndex = static_cast<size_t>(hit);
          activateSelected();
          return;
        }
      }
    }
    if (mode != Mode::Grid && (snap.touchPressed || snap.touchReleased)) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  const int listSize = static_cast<int>(rowCount());

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    goUp();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    switchTab(static_cast<int>(tab) - 1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    switchTab(static_cast<int>(tab) + 1);
    return;
  }

  buttonNavigator.onNextRelease([this, listSize] {
    moveSelection(ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize));
  });
  buttonNavigator.onPreviousRelease([this, listSize] {
    moveSelection(ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize));
  });
  buttonNavigator.onNextContinuous([this, listSize] {
    moveSelection(ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, visibleRows));
  });
  buttonNavigator.onPreviousContinuous([this, listSize] {
    moveSelection(ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, visibleRows));
  });
}

void LibraryActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<LibraryActivity*>(user)->buildListScreen(screen);
}

void LibraryActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int headerBottom = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput);
  const int tabH = renderer.getLineHeight(UI_10_FONT_ID) + 12;
  const int statusH = renderer.getLineHeight(SMALL_FONT_ID) + 8;
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(headerBottom + tabH), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight + statusH), 0});

  const size_t total = rowCount();
  if (!indexReady || total == 0) {
    const char* message = !indexReady ? (buildFailed ? tr(STR_ERROR_GENERAL_FAILURE) : tr(STR_LIBRARY_EMPTY))
                                      : tr(STR_LIBRARY_EMPTY);
    screen.centeredText(message, screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.labelText = screen.theme().bodyText;
  props.labelText.bold = true;
  const UiListRowType rowType = mode == Mode::Groups ? UiListRowType::SingleLine : UiListRowType::WithSubtitle;
  props.labelText.maxLines = mode == Mode::Groups ? 1 : 2;
  const fui::Rect listRect = screen.body();
  const auto rows = configureUiList(props, screen.theme(), listRect, rowType);
  visibleRows = rows > 0 ? rows : 1;

  listNav.selected = static_cast<int>(selectorIndex);
  listNav.top = topIndex;
  listNav.visibleRows = visibleRows;
  listNav.syncToProps(listRect, props.rowHeight, props.rowGap, static_cast<int>(total), props);
  topIndex = listNav.top;

  const size_t drawCount = std::min<size_t>(visibleRows, total - static_cast<size_t>(topIndex));
  std::vector<std::string> labels(drawCount);
  std::vector<std::string> subtitles(drawCount);
  std::vector<std::string> values(drawCount);
  std::vector<fui::ListItem> itemsOut;
  itemsOut.reserve(drawCount);

  for (size_t i = 0; i < drawCount; i++) {
    const size_t row = static_cast<size_t>(topIndex) + i;
    fui::ListItem item;
    if (mode == Mode::Groups) {
      labels[i] = groups[row].name;
      values[i] = std::to_string(groups[row].count);
      item.icon = listIconFor(UIIcon::BookOpenText, 24);
    } else {
      uint16_t ordinal = 0xFFFF;
      if (rowOrdinal(row, ordinal)) {
        library::ClixRecord record{};
        if (index.readRecord(ordinal, record)) {
          std::string name;
          index.readName(record, name);
          if (!index.readTitle(record, labels[i])) labels[i] = name;
          if (!index.readAuthor(record, subtitles[i])) index.readSeries(record, subtitles[i]);
        }
      }
      item.icon = listIconFor(UIIcon::Book, 32);
    }
    item.label = labels[i].c_str();
    if (!subtitles[i].empty()) item.subtitle = subtitles[i].c_str();
    if (!values[i].empty()) item.value = values[i].c_str();
    item.actionValue = static_cast<int16_t>(row);
    itemsOut.push_back(item);
  }

  props.items = itemsOut.data();
  props.count = static_cast<uint16_t>(total);
  props.itemsWindowFirst = static_cast<uint16_t>(topIndex);
  props.itemsWindowCount = static_cast<uint16_t>(drawCount);
  props.action = ACTION_ROW;
  props.inputMask = static_cast<uint16_t>(fui::InputTouch);
  props.valueInset = 8;
  props.balanceWrappedLabelWithValue = false;
  props.partialTrailingRow = false;
  screen.list(props);
  topIndex = listNav.top;

  const auto indicatorRows = static_cast<uint16_t>(listNav.pageRowsFor(static_cast<int>(total)));
  fui::drawListScrollIndicator(screen.target(), listRect, total, indicatorRows, topIndex,
                               screen.theme().listScrollWidth, screen.theme().listScrollSide,
                               screen.theme().listScrollInset);
}

void LibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_MENU_LIBRARY), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_MENU_LIBRARY));
  }

  // Draw the header search icon for every tab (the grid tab does not go through
  // the FreeInkApp screen builder, so it must be drawn here).
  if (mappedInput.hasTouchHardware()) {
    const Rect search = searchIconRect();
    drawLucideIcon(renderer, icon_search_24, search.x + (search.width - icon_search_24.w) / 2,
                   search.y + (search.height - icon_search_24.h) / 2);
    const Rect filterRect = filterIconRect();
    drawLucideIcon(renderer, icon_filter_24, filterRect.x + (filterRect.width - icon_filter_24.w) / 2,
                   filterRect.y + (filterRect.height - icon_filter_24.h) / 2);
  }

  if (!indexReady) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, CompactHeader::contentTop(metrics) + 20,
                      buildFailed ? tr(STR_ERROR_GENERAL_FAILURE) : tr(STR_LIBRARY_EMPTY));
  } else if (mode == Mode::Grid) {
    drawGrid();
  } else {
    uiReady = false;
    app.render();
    uiReady = true;
  }
  drawTabs();

  const int statusH = renderer.getLineHeight(SMALL_FONT_ID) + 8;
  const int statusY = renderer.getScreenHeight() - metrics.buttonHintsHeight - statusH + 4;
  if (drilled && !groupName.empty()) {
    const std::string truncated =
        renderer.truncatedText(SMALL_FONT_ID, groupName.c_str(), renderer.getScreenWidth() / 2, EpdFontFamily::REGULAR);
    const int w = renderer.getTextWidth(SMALL_FONT_ID, truncated.c_str(), EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, (renderer.getScreenWidth() - w) / 2, statusY, truncated.c_str(), true,
                      EpdFontFamily::REGULAR);
  }
  const int total = static_cast<int>(rowCount());
  if (total > 0 && mode != Mode::Groups) {
    char countBuf[48];
    snprintf(countBuf, sizeof(countBuf), tr(STR_BOOKS_COUNT), static_cast<int>(selectorIndex) + 1, total);
    const int countW = renderer.getTextWidth(SMALL_FONT_ID, countBuf, EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - metrics.contentSidePadding - countW, statusY,
                      countBuf, true, EpdFontFamily::REGULAR);
  } else if (total > 0) {
    char countBuf[48];
    snprintf(countBuf, sizeof(countBuf), "%d", total);
    const int countW = renderer.getTextWidth(SMALL_FONT_ID, countBuf, EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - metrics.contentSidePadding - countW, statusY,
                      countBuf, true, EpdFontFamily::REGULAR);
  }

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_HOME)), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (indexReady && mode == Mode::Grid) {
    ensurePageCovers();
  }
}
