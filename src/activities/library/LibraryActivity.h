#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <I18n.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <LibraryIndexFile.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Card-wide shelf backed by the CLX1 library index.
//
// Tabs (CrossPoint's layout, plus a Series tab):
//   Recent - a list of books, most recently added first.
//   Title  - a cover grid of every book.
//   Author - one entry per author; selecting opens that author's books.
//   Series - one entry per series; selecting opens that series' books, in
//            series order.
//
// A non-empty search collapses every tab to one flat list of matching books.
class LibraryActivity final : public Activity {
 private:
  using UiApp = freeink::ui::FreeInkApp<24, 8>;

  enum class Tab : uint8_t { Recent, Title, Author, Series };
  enum class Mode : uint8_t { List, Grid, Groups };
  enum class Filter : uint8_t { All, Unread, Finished };

  // One aggregated author / series, as a run of the active sort permutation.
  struct Group {
    std::string name;
    uint16_t firstRow = 0;
    uint16_t count = 0;
  };

  static constexpr int kTabCount = 4;
  static constexpr int kNoPageLoaded = -1;

  // Grid geometry is derived per view: 3x3 for the Title tab, 2x2 (larger
  // covers) when drilled into a series.
  int gridCols = 3;
  int gridRows = 3;
  int coverWidth = 123;
  int coverHeight = 180;

  ButtonNavigator buttonNavigator;

  library::LibraryIndexFile index;
  bool indexReady = false;
  bool buildFailed = false;

  Tab tab = Tab::Recent;
  Mode mode = Mode::List;
  Filter filter = Filter::All;
  bool reversed = false;
  bool drilled = false;

  std::string query;      // non-empty while searching
  std::string groupName;  // author/series being shown in a drilled list

  std::vector<uint16_t> items;  // ordinals for List / Grid modes
  std::vector<Group> groups;    // Groups mode

  size_t selectorIndex = 0;
  int topIndex = 0;
  int visibleRows = 1;
  int listTop = 0;
  int loadedPageStart = kNoPageLoaded;
  bool backLongPressFired = false;
  bool confirmLongPressFired = false;

  freeink::ui::ListNav listNav;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`
  UiApp app;
  std::atomic<bool> uiReady{false};

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);

  void ensureIndex();
  void rebuildContent();
  void buildSearch();              // global book search (title/author/series)
  void buildGroups(bool byAuthor);
  void openGroup(size_t groupIndex);

  size_t rowCount() const;
  bool rowOrdinal(size_t row, uint16_t& ordinal);

  void switchTab(int tabIndex);
  void toggleReverse();
  void openSearch();
  void openFilterMenu();
  bool passesFilter(const library::ClixRecord& record) const;
  void moveSelection(int index);
  void activateSelected();
  void goUp();

  Rect tabBandRect() const;
  Rect contentRect() const;
  Rect searchIconRect() const;
  Rect filterIconRect() const;
  int tabIndexFromPoint(int x, int y) const;
  int gridIndexFromPoint(int x, int y);
  void updateGridGeometry();
  int booksPerPage() const { return gridCols * gridRows; }
  void drawTabs() const;
  void drawGrid();
  void ensurePageCovers();

 public:
  explicit LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
