// SPDX-License-Identifier: Apache-2.0
// Every member of Sheet and Workbook that schema/model_fields.yaml calls saved comes back from a .ycg file as it was:
// a workbook with something in every member is saved and opened, then each member is compared. A member nobody
// writes to the file (the calculation mode, iterative calculation and the outline settings were lost that way)
// fails here, and scripts/check_model_fields.py fails while a saved member isn't compared here.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "io/content_dump.h"
#include "io/ycg.h"

#include <string>

namespace model = yc::model;
namespace engine = yc::engine;
namespace io = yc::io;
using yc::core::Rect;
using yc::core::Value;

namespace {

// a workbook with something in every member that is saved
model::Workbook RichWorkbook() {
  yctest::Doc t;
  engine::Document& d = t.d;
  const uint32_t sh = t.sheet();
  const uint32_t pv = d.AddSheet("Report");
  // cells, formulas, formats, rows and columns, merges, whole-row and whole-column formats
  t.Set("A1", "Region");
  t.Set("B1", "Sales");
  t.Set("A2", "East");
  t.Set("B2", "10");
  t.Set("A3", "West");
  t.Set("B3", "20");
  t.Set("C2", "=B2*2");
  t.Set("E1", "hello");
  d.SetRowHeight(sh, 5, 5, 40);
  d.SetColWidth(sh, 7, 7, 120);
  REQUIRE(d.Merge(sh, Rect{9, 0, 9, 2}));
  {
    std::string err;
    REQUIRE(d.SetArrayFormula(sh, Rect{0, 10, 1, 10}, "{1;2}", &err));  // K1:K2, an array (a spill of its own)
  }
  model::Sheet& s = *d.wb_mut().MutSheetById(sh);
  s.row_styles.mut()[20] = 0;
  s.col_styles.mut()[20] = 0;
  s.code_name = "Sheet1Code";
  s.tab_color = 0xFF00FF00;
  s.outline_below = false;
  s.outline_right = false;
  s.outline_styles = true;
  s.tab_selected = true;
  {
    model::Control c;
    c.kind = model::ControlKind::CheckBox;
    c.linked_cell = "$H$1";
    d.AddControl(sh, c);
  }
  {
    model::ChartObj c;
    c.source = "Sheet1!$A$1:$B$3";
    c.series.emplace_back();
    c.series[0].values = "Sheet1!$B$2:$B$3";
    c.series[0].xerr_plus = "Sheet1!$C$2:$C$3";
    c.title = "Sales";
    d.AddChart(sh, c);
  }
  d.SetNote(sh, 3, 3, "Ann", "a note");
  d.SetHyperlink(sh, 4, 4, model::Hyperlink{"https://example.com/", "", "tip"});
  s.ignored_errors.mut().push_back(Rect{1, 1, 2, 1});
  {
    model::RichText rt;
    rt.text = "hello";
    model::RichRun run;
    run.start = 0;
    run.bold = 1;
    rt.runs.push_back(run);
    model::RichRun rest;
    rest.start = 2;
    rt.runs.push_back(rest);
    d.SetRichText(sh, 0, 4, rt);
  }
  s.store.mut()["k"] = std::string("\xa1v", 2);  // a msgpack string "v"
  {
    model::CfRule r;
    r.ranges.push_back(Rect{1, 1, 2, 1});
    r.type = model::CfType::CellValue;
    r.formula1 = "15";
    d.SetCondFormats(sh, {r});
  }
  {
    model::DvRule r;
    r.ranges.push_back(Rect{1, 5, 3, 5});
    r.type = model::DvType::List;
    r.formula1 = "Buy,Sell";
    model::Sheet& m = *d.wb_mut().MutSheetById(sh);
    m.validations.mut().push_back(r);
  }
  REQUIRE(d.SetAutoFilter(sh, Rect{0, 0, 2, 1}));
  {
    model::Sheet& m = *d.wb_mut().MutSheetById(sh);
    m.protection.mut().ranges.push_back(model::AllowEditRange{"R1", Rect{1, 1, 2, 1}, 0, {}});
    m.protection.mut().format_cells = true;
  }
  REQUIRE(d.AddTable(sh, Rect{12, 0, 14, 1}, true, "TableStyleMedium2") != 0);
  {
    model::ShapeObj sp;
    sp.kind = model::ShapeKind::Rectangle;
    sp.text = "box";
    const uint32_t id = d.AddShape(sh, sp);
    model::Sheet& m = *d.wb_mut().MutSheetById(sh);
    m.linked_pictures.mut().push_back(model::Sheet::LinkedPicture{id, sh, Rect{1, 1, 2, 1}});
  }
  {
    model::SparklineGroup g;
    g.items.push_back(model::Sparkline{1, 3, sh, Rect{1, 1, 1, 2}});
    g.date_range = "$A$2:$A$3";
    d.AddSparklineGroup(sh, g);
  }
  d.UpdatePageSetup(sh, [](model::PageSetup& ps) {
    ps.has_area = true;
    ps.area = Rect{0, 0, 9, 3};
    ps.header_center = "&A";
  });
  d.wb_mut().MutSheetById(sh)->background.reset(std::string("BMbytes"));
  {
    model::PivotTable pt;
    pt.src_sheet = sh;
    pt.src = Rect{0, 0, 2, 1};
    pt.rows.push_back(model::PivotAxisField{});
    pt.rows[0].field = 0;
    pt.data.push_back(model::PivotDataField{});
    pt.data[0].field = 1;
    std::string err;
    REQUIRE(d.AddPivot(pv, pt, &err) != 0);
    model::Slicer sl;
    sl.pivot = d.S(pv)->pivots->at(0).name;
    sl.field = 0;
    REQUIRE(d.AddSlicer(pv, sl) != 0);
  }
  {
    const yc::core::CellPos in{1, 1};
    t.Set("H20", "=B2*3");
    std::string err;
    REQUIRE(d.AddDataTable(sh, Rect{19, 6, 21, 7}, nullptr, &in, &err));  // G20:H22, column input B2
    model::Scenario sc;
    sc.name = "High";
    sc.cells.push_back(yc::core::CellPos{1, 1});
    sc.values.push_back(Value::Number(99));
    REQUIRE(d.AddScenario(sh, sc, &err));
  }
  {
    model::Sheet& m = *d.wb_mut().MutSheetById(sh);
    model::SolverModel sm;
    sm.defined = true;
    sm.objective = "$C$2";
    sm.changing = "$B$2";
    m.solver.reset(sm);
    model::ConsolidateSettings cs;
    cs.func = 1;
    cs.refs.push_back("Sheet1!$A$1:$B$3");
    m.consolidate.reset(cs);
    m.view.freeze_row = 1;
    m.view.act_row = 2;
  }
  // a hidden sheet, a very hidden one and a chart sheet
  {
    const uint32_t h = d.AddSheet("Hidden");
    d.wb_mut().MutSheetById(h)->visible = false;
    const uint32_t vh = d.AddSheet("VeryHidden");
    d.wb_mut().MutSheetById(vh)->visible = false;
    d.wb_mut().MutSheetById(vh)->very_hidden = true;
    model::ChartObj c;
    c.series.emplace_back();
    c.series[0].values = "Sheet1!$B$2:$B$3";
    d.AddChartSheet("Chart1", -1, c);
  }
  // the workbook's own members
  model::Workbook& wb = d.wb_mut();
  {
    model::NameDef n;
    n.name = "Rate";
    n.refers_to = "=Sheet1!$B$2";
    n.comment = "the rate";
    REQUIRE(d.DefineName(n));
  }
  wb.store.mut()["wk"] = std::string("\xa1w", 2);
  wb.doc_props.mut()["Owner"] = model::DocProp{Value::String("desk"), false, ""};
  {
    model::Panel p;
    p.name = "Trade";
    p.controls.emplace_back();
    p.controls[0].type = "button";
    p.controls[0].name = "Go";
    wb.panels.mut().push_back(p);
    model::ContainerDef c;
    c.name = "Box";
    wb.containers.mut().push_back(c);
  }
  {
    model::Theme th = *wb.theme;
    th.name = "Custom";
    wb.theme.reset(th);
    model::NamedStyle ns;
    ns.name = "Mine";
    wb.cell_styles.mut().push_back(ns);
  }
  {
    model::ExternalLink l;
    l.path = "C:\\data\\prices.ycg";
    l.sheets.push_back("Q");
    l.values.emplace_back();
    l.values[0][1] = Value::Number(3);
    wb.external_links.mut().push_back(l);
    model::BoundTable bt;
    bt.name = "Feed";
    bt.key = "sym";
    wb.bound_tables.mut().push_back(bt);
  }
  {
    model::ParameterDef p;
    p.sheet = sh;
    p.row = 1;
    p.col = 1;
    p.name = "Qty";
    wb.parameters.mut().push_back(p);
    model::AuditEntry e;
    e.user = "ann";
    e.kind = "parameter";
    wb.audit_log.mut().push_back(e);
    model::SafetySettings ss = *wb.safety;
    ss.kill_macro = "panic";
    wb.safety.reset(ss);
    model::AlertRule a;
    a.name = "Big";
    a.sheet = sh;
    a.range = Rect{1, 1, 2, 1};
    wb.alerts.mut().push_back(a);
  }
  wb.notebook.reset({"print(1)"});
  wb.layout_json = "{\"panes\":1}";
  wb.protect_structure = true;
  wb.protect_windows = true;
  wb.protect_hash = 0x1234;
  wb.calc_iterate = true;
  wb.calc_max_iterations = 42;
  wb.calc_max_change = 0.5;
  wb.calc_mode = 2;
  wb.date1904 = true;
  wb.precision_as_displayed = true;
  wb.force_full_calc = true;
  wb.active_sheet = 1;
  {
    model::RawChunk u;
    u.fourcc = io::FourCC("ZZZZ");
    u.version = 1;
    u.flags = io::kChunkPreserve;  // kept and written back by a version that can't read it
    u.data = "future";
    wb.unknown_chunks.mut().push_back(u);
  }
  return d.wb();
}

}  // namespace

TEST_CASE("Every saved member of the workbook and its sheets comes back from a .ycg file as it was") {
  const model::Workbook a = RichWorkbook();
  const std::string bytes = io::SaveYcgToBytes(a, io::SaveOptions{});
  io::LoadResult lr = io::LoadYcgFromBytes(bytes);
  REQUIRE(lr.ok);
  const model::Workbook& b = lr.wb;

  // the cells (values, formulas, formats), rows and columns, merged areas and whole-row / whole-column formats:
  // through the dump of the content (a.values, a.meta, a.rows, a.cols, a.merges, a.row_styles, a.col_styles,
  // a.styles, a.formulas)
  {
    const std::string da = io::ContentDump(a), db = io::ContentDump(b);
    if (da != db) {  // name the first line that differs
      size_t i = 0;
      while (i < da.size() && i < db.size() && da[i] == db[i]) ++i;
      const size_t nl = da.rfind('\n', i), from = nl == std::string::npos ? 0 : nl + 1;
      INFO("saved: " << da.substr(from, da.find('\n', from) - from));
      INFO("opened: " << db.substr(from, db.find('\n', from) - from));
      CHECK(da == db);
    }
  }

#define SAME(x)          \
  do {                   \
    INFO(#x);            \
    CHECK(a.x == b.x);   \
  } while (0)
#define SAME_COW(x)      \
  do {                   \
    INFO(#x);            \
    CHECK(*a.x == *b.x); \
  } while (0)

  REQUIRE(a.sheets.size() == b.sheets.size());
  const model::Workbook& wa = a;
  const model::Workbook& wb = b;
  for (size_t i = 0; i < wa.sheets.size(); ++i) {
    INFO("sheet " << wa.sheets[i]->name);
    [&](const model::Sheet& a, const model::Sheet& b) {  // the member names below read as a.member / b.member
      SAME(id);
      SAME(name);
      SAME(code_name);
      SAME(row_limit);
      SAME(col_limit);
      SAME(visible);
      SAME(very_hidden);
      SAME(tab_color);
      SAME(chart_sheet);
      SAME(outline_below);
      SAME(outline_right);
      SAME(outline_styles);
      SAME_COW(controls);
      SAME_COW(charts);
      SAME_COW(notes);
      SAME_COW(ignored_errors);
      SAME_COW(links);
      SAME_COW(rich);
      SAME_COW(store);
      SAME_COW(cond_formats);
      SAME_COW(validations);
      SAME_COW(autofilter);
      SAME_COW(spills);
      SAME_COW(protection);
      SAME_COW(tables);
      SAME_COW(shapes);
      SAME_COW(linked_pictures);
      SAME(tab_selected);
      SAME_COW(sparklines);
      SAME_COW(page_setup);
      SAME_COW(background);
      {
        // a report's notes of its last refresh aren't saved (pivot.h: written, written_rows, written_cols, fields_src)
        auto kept = [](std::vector<model::PivotTable> v) {
          for (auto& p : v) {
            p.written.clear();
            p.written_rows.clear();
            p.written_cols.clear();
            p.fields_src = -1;
          }
          return v;
        };
        INFO("pivots");
        CHECK(kept(*a.pivots) == kept(*b.pivots));
      }
      SAME_COW(slicers);
      SAME_COW(data_tables);
      SAME_COW(scenarios);
      SAME_COW(solver);
      SAME_COW(consolidate);
      SAME(view);  // its zoom, gridlines and headings are the window's, kept in layout_json
    }(*wa.sheets[i], *wb.sheets[i]);
  }
  // the sheets with something in them really have it (a member left empty would compare equal for nothing)
  const model::Sheet& s1 = *a.sheets[0];
  CHECK(!s1.controls->empty());
  CHECK(!s1.charts->empty());
  CHECK(!s1.notes->empty());
  CHECK(!s1.links->empty());
  CHECK(!s1.rich->empty());
  CHECK(!s1.spills->empty());
  CHECK(!s1.tables->empty());
  CHECK(!s1.shapes->empty());
  CHECK(!s1.sparklines->empty());
  CHECK(!s1.data_tables->empty());
  CHECK(!s1.scenarios->empty());
  CHECK(!a.sheets[1]->pivots->empty());
  CHECK(!a.sheets[1]->slicers->empty());

  SAME(next_sheet_id);
  SAME(next_object_id);
  SAME_COW(names);
  SAME_COW(store);
  SAME_COW(doc_props);
  SAME_COW(panels);
  SAME_COW(containers);
  SAME_COW(theme);
  SAME_COW(cell_styles);
  SAME_COW(external_links);
  SAME_COW(bound_tables);
  SAME_COW(parameters);
  SAME_COW(audit_log);
  SAME_COW(safety);
  SAME_COW(alerts);
  SAME_COW(notebook);
  SAME(layout_json);
  SAME(protect_structure);
  SAME(protect_windows);
  SAME(protect_hash);
  SAME(calc_iterate);
  SAME(calc_max_iterations);
  SAME(calc_max_change);
  SAME(calc_mode);
  SAME(date1904);
  SAME(precision_as_displayed);
  SAME(force_full_calc);
  SAME(active_sheet);
  SAME_COW(unknown_chunks);
#undef SAME
#undef SAME_COW
}
