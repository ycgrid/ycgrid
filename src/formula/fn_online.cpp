// SPDX-License-Identifier: Apache-2.0
// fn_online.cpp - functions that use data from the web, and language detection: STOCKHISTORY TRANSLATE DETECTLANGUAGE
//
// Excel takes STOCKHISTORY's prices and TRANSLATE's translations from Microsoft services. YCGrid downloads them in the
// background like WEBSERVICE (the cell shows #GETTING_DATA until they arrive): prices from Yahoo Finance's public chart
// data, translations from Google's public translation endpoint. DETECTLANGUAGE works offline: the writing system, then
// the most frequent words of the Latin-script languages.
#include "formula/fn_util.h"

#include "core/i18n.h"
#include "formula/datetime.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace yc::formula::fn {
namespace {

using ArrP = std::shared_ptr<Array>;
using nlohmann::json;

std::string UrlEncode(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      o += static_cast<char>(c);
    } else {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", c);
      o += b;
    }
  }
  return o;
}

std::string Upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// a ticker as Excel writes it ("MSFT", "XNAS:MSFT", "XTAI:2330") -> Yahoo's symbol ("MSFT", "2330.TW")
std::string YahooSymbol(std::string t) {
  t.erase(std::remove(t.begin(), t.end(), ' '), t.end());
  const size_t colon = t.find(':');
  if (colon == std::string::npos) {
    // a Taiwan stock number alone ("2330") is listed on the Taiwan Stock Exchange
    if (!t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }) && t.size() >= 4) return t + ".TW";
    return Upper(t);
  }
  const std::string mic = Upper(t.substr(0, colon)), code = Upper(t.substr(colon + 1));
  static const std::map<std::string, std::string> kSuffix = {
      {"XTAI", ".TW"}, {"ROCO", ".TWO"}, {"XTKS", ".T"}, {"XHKG", ".HK"}, {"XSHG", ".SS"}, {"XSHE", ".SZ"}, {"XLON", ".L"},  {"XPAR", ".PA"},
      {"XETR", ".DE"}, {"XFRA", ".F"},   {"XAMS", ".AS"}, {"XMIL", ".MI"}, {"XMAD", ".MC"}, {"XSWX", ".SW"}, {"XTSE", ".TO"}, {"XASX", ".AX"},
      {"XKRX", ".KS"}, {"XKOS", ".KQ"},  {"XSES", ".SI"}, {"XBOM", ".BO"}, {"XNSE", ".NS"}, {"XSTO", ".ST"}, {"XOSL", ".OL"}, {"XCSE", ".CO"},
      {"XHEL", ".HE"}, {"XBRU", ".BR"},  {"XWBO", ".VI"}, {"XNZE", ".NZ"}, {"BVMF", ".SA"}, {"XMEX", ".MX"}, {"XJSE", ".JO"}};
  const auto it = kSuffix.find(mic);
  return it != kSuffix.end() ? code + it->second : code;  // XNAS, XNYS, ARCX...: the ticker itself
}

// a member or an element of an answer that may lack it: nlohmann's const operator[] asserts (and reads past the map
// in a release build) when the key isn't there, and Yahoo leaves "timestamp" out of a range without trading days
const json& Member(const json& j, const char* key) {
  static const json kNull;
  if (!j.is_object()) return kNull;
  const auto it = j.find(key);
  return it != j.end() ? *it : kNull;
}
const json& Element(const json& j, size_t i) {
  static const json kNull;
  return j.is_array() && i < j.size() ? j[i] : kNull;
}

double ToSerial(Evaluator& ev, const Node& n, bool* ok) {
  const Value v = ev.Scalar(n);
  *ok = true;
  if (v.IsNumeric()) return std::floor(v.AsDouble());
  if (v.IsString()) {
    double d = 0;
    if (ParseDateText(v.str(), &d)) return std::floor(d);
  }
  *ok = false;
  return 0;
}

// =STOCKHISTORY(stock, start_date, [end_date], [interval], [headers], [property0], ...)
EV FStockHistory(Evaluator& ev, const Node& call) {
  YC_TEXT(stock, 0);
  bool ok = false;
  double start = ToSerial(ev, call.kids[1], &ok);
  if (!ok || stock.empty()) return Er(Err::Value);
  double end = start;
  if (Has(call, 2)) {
    end = ToSerial(ev, call.kids[2], &ok);
    if (!ok) return Er(Err::Value);
  }
  if (end < start) std::swap(end, start);
  YC_NUM_OPT(interval, 3, 0);
  YC_NUM_OPT(headers, 4, 1);
  if (interval < 0 || interval > 2 || headers < 0 || headers > 2) return Er(Err::Value);
  std::vector<int> props;
  for (size_t i = 5; i < call.kids.size(); ++i) {
    if (!Has(call, static_cast<int>(i))) continue;
    const Value p = ev.Scalar(call.kids[i]);
    if (!p.IsNumeric() || p.AsDouble() < 0 || p.AsDouble() > 5) return Er(Err::Value);
    props.push_back(static_cast<int>(p.AsDouble()));
  }
  if (props.empty()) props = {0, 1};  // Date, Close
  const auto epoch = [](double serial) { return static_cast<long long>((serial - EpochSerial()) * 86400.0); };
  static const char* kInterval[] = {"1d", "1wk", "1mo"};
  char url[512];
  std::snprintf(url, sizeof url, "https://query1.finance.yahoo.com/v8/finance/chart/%s?period1=%lld&period2=%lld&interval=%s&events=history",
                UrlEncode(YahooSymbol(stock)).c_str(), epoch(start) - 86400, epoch(end) + 2 * 86400, kInterval[static_cast<int>(interval)]);
  const EV r = ev.host().WebFetch(url);
  if (r.k != EV::K::Scalar || !r.s.IsString()) return r;  // still downloading, or failed
  json j = json::parse(std::string(r.s.str()), nullptr, false);
  if (j.is_discarded()) return Er(Err::Value);
  const json& res = Member(Member(j, "chart"), "result");
  if (!res.is_array() || res.empty()) return Er(Err::Value);
  const json& d = res[0];
  const json& gmt = Member(Member(d, "meta"), "gmtoffset");
  const long long off = gmt.is_number_integer() ? gmt.get<long long>() : 0LL;
  const json& ts = Member(d, "timestamp");
  const json& q = Element(Member(Member(d, "indicators"), "quote"), 0);
  if (!ts.is_array() || !q.is_object()) return Er(Err::Value);
  static const char* kField[] = {"", "close", "open", "high", "low", "volume"};
  static const char* kHeader[] = {"Date", "Close", "Open", "High", "Low", "Volume"};
  std::vector<std::vector<Value>> rows;
  for (size_t i = 0; i < ts.size(); ++i) {
    // the trading day in the exchange's time zone
    if (!ts[i].is_number()) continue;
    const double day = std::floor((ts[i].get<double>() + static_cast<double>(off)) / 86400.0) + EpochSerial();
    if (day < start || day > end) {
      // weekly and monthly rows start before the requested start date: keep a row that covers it
      if (interval == 0 || day > end) continue;
    }
    std::vector<Value> row;
    bool any = false;
    for (int p : props) {
      if (p == 0) {
        row.push_back(Value::Number(day));
        continue;
      }
      const json& col = Member(q, kField[p]);
      if (col.is_array() && i < col.size() && col[i].is_number()) {
        double v = col[i].get<double>();
        if (p != 5) v = std::round(v * 10000) / 10000;  // prices as the exchange lists them (no float noise)
        row.push_back(Value::Number(v));
        any = true;
      } else {
        row.push_back(Value());
      }
    }
    if (any) rows.push_back(std::move(row));
  }
  if (rows.empty()) return Er(Err::Value);
  auto a = std::make_shared<Array>();
  a->cols = static_cast<uint32_t>(props.size());
  if (headers == 2) {
    a->v.push_back(Value::String(Upper(stock)));
    for (size_t k = 1; k < props.size(); ++k) a->v.push_back(Value::String(""));
    ++a->rows;
  }
  if (headers >= 1) {
    for (int p : props) a->v.push_back(Value::String(kHeader[p]));
    ++a->rows;
  }
  for (auto& row : rows) {
    for (auto& v : row) a->v.push_back(std::move(v));
    ++a->rows;
  }
  return EV::A(std::move(a));
}

// =TRANSLATE(text, [source_language], [target_language])
EV FTranslate(Evaluator& ev, const Node& call) {
  YC_TEXT(text, 0);
  if (text.empty()) return EV::S(Value::String(""));
  std::string src = "auto", dst = i18n::Current() == i18n::Lang::ZhTW ? "zh-TW" : "en";
  if (Has(call, 1)) {
    YC_TEXT(s, 1);
    if (!s.empty()) src = s;
  }
  if (Has(call, 2)) {
    YC_TEXT(t, 2);
    if (!t.empty()) dst = t;
  }
  if (text.size() > 5000 || src.size() > 12 || dst.size() > 12) return Er(Err::Value);
  const std::string url = "https://translate.googleapis.com/translate_a/single?client=gtx&dt=t&sl=" + UrlEncode(src) + "&tl=" + UrlEncode(dst) + "&q=" + UrlEncode(text);
  const EV r = ev.host().WebFetch(url);
  if (r.k != EV::K::Scalar || !r.s.IsString()) return r;
  json j = json::parse(std::string(r.s.str()), nullptr, false);
  if (j.is_discarded() || !j.is_array() || j.empty() || !j[0].is_array()) return Er(Err::Value);
  std::string out;
  for (const auto& seg : j[0]) {
    if (seg.is_array() && !seg.empty() && seg[0].is_string()) out += seg[0].get<std::string>();
  }
  return EV::S(Value::String(out));
}

// ---- DETECTLANGUAGE: offline ----

// UTF-8 -> code points
std::vector<uint32_t> CodePoints(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    int n = 0;
    if (c >= 0xF0) cp = c & 0x07, n = 3;
    else if (c >= 0xE0) cp = c & 0x0F, n = 2;
    else if (c >= 0xC0) cp = c & 0x1F, n = 1;
    ++i;
    for (int k = 0; k < n && i < s.size(); ++k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    out.push_back(cp);
  }
  return out;
}

std::string LowerUtf8Ascii(const std::string& s) {
  std::string o = s;
  for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

std::string DetectLanguage(const std::string& text) {
  const auto cps = CodePoints(text);
  std::map<std::string, int> script;
  int simp = 0, trad = 0;
  // characters that differ between simplified and traditional Chinese (a small, frequent set)
  static const std::u32string kSimp = U"们这个说时对会来国为过发后见长门问间东车马鸟书学习实现电话语读写钱银图觉经还进开关边听运动处无变应总让";
  static const std::u32string kTrad = U"們這個說時對會來國為過發後見長門問間東車馬鳥書學習實現電話語讀寫錢銀圖覺經還進開關邊聽運動處無變應總讓";
  for (uint32_t c : cps) {
    if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF)) ++script["kana"];
    else if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF)) ++script["hangul"];
    else if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF)) {
      ++script["han"];
      if (kSimp.find(static_cast<char32_t>(c)) != std::u32string::npos) ++simp;
      if (kTrad.find(static_cast<char32_t>(c)) != std::u32string::npos) ++trad;
    } else if (c >= 0x0400 && c <= 0x04FF) ++script["cyrillic"];
    else if (c >= 0x0370 && c <= 0x03FF) ++script["greek"];
    else if (c >= 0x0600 && c <= 0x06FF) ++script["arabic"];
    else if (c >= 0x0590 && c <= 0x05FF) ++script["hebrew"];
    else if (c >= 0x0E00 && c <= 0x0E7F) ++script["thai"];
    else if (c >= 0x0900 && c <= 0x097F) ++script["devanagari"];
    else if (c >= 0x0980 && c <= 0x09FF) ++script["bengali"];
    else if (c >= 0x0B80 && c <= 0x0BFF) ++script["tamil"];
    else if (c >= 0x10A0 && c <= 0x10FF) ++script["georgian"];
    else if (c >= 0x0530 && c <= 0x058F) ++script["armenian"];
    else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0x24F)) ++script["latin"];
  }
  if (script.empty()) return "";
  // Japanese mixes kana with kanji; Chinese has none
  if (script["kana"] > 0) return "ja";
  if (script["hangul"] > 0) return "ko";
  const std::string top = std::max_element(script.begin(), script.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
  if (top == "han") return simp > trad ? "zh-Hans" : "zh-Hant";
  auto has = [&](const std::u32string& chars) {
    for (uint32_t c : cps) {
      if (chars.find(static_cast<char32_t>(c)) != std::u32string::npos) return true;
    }
    return false;
  };
  if (top == "cyrillic") {
    if (has(U"їєґі")) return "uk";
    if (has(U"ўі")) return "be";
    if (has(U"ђћџљњ")) return "sr";
    return "ru";
  }
  if (top == "arabic") return has(U"پچژگ") ? "fa" : (has(U"ٹڈڑںے") ? "ur" : "ar");
  static const std::map<std::string, std::string> kScript = {{"greek", "el"},  {"hebrew", "he"},   {"thai", "th"},     {"devanagari", "hi"},
                                                             {"bengali", "bn"}, {"tamil", "ta"},   {"georgian", "ka"}, {"armenian", "hy"}};
  if (auto it = kScript.find(top); it != kScript.end()) return it->second;
  // Latin script: the language whose frequent words and letters match best
  struct Profile {
    const char* code;
    const char* words;
    std::u32string letters;
  };
  static const Profile kProfiles[] = {
      {"en", " the and of to is in that it for you was with on are this be have not are they at from or by but what all were we when your can said there use an each which she do how their if will up other about out many then them these so some her would make like him into time has look two more write go see no way could people my than first been who its now find long down day did get come made may part ", U""},
      {"fr", " le la les de des et est un une que qui dans pour pas sur au aux ce cette il elle nous vous ils sont avec plus par ne se en du mais ou je tu son sa ses leur être avoir fait comme tout bien ", U"éèêàçùâîôûœ"},
      {"de", " der die das und ist nicht ein eine zu den von mit sich des auf für im dem als auch es an werden aus er hat dass sie nach wird bei einer um am sind noch wie einem über einen so zum war haben nur oder aber vor zur bis mehr durch man ich du wir ihr ", U"äöüß"},
      {"es", " el la los las de y que en un una es por con para no se su al lo como más pero sus le ya o este sí porque esta entre cuando muy sin sobre también me hasta hay donde quien desde todo nos durante todos uno les ni contra otros ese eso ", U"ñáéíóúü¿¡"},
      {"it", " il lo la i gli le di e che è un una per non in con su del della dei delle al alla da come più ma anche sono questo questa ci si mi ti ha hanno essere fare tutto ", U"àèéìòù"},
      {"pt", " o a os as de e que é um uma para com não no na nos nas do da dos das por se em ao mais mas como foi ele ela eles elas seu sua você eu tem são está muito também já ", U"ãõçáéíóúâêô"},
      {"nl", " de het een en van is dat niet in te op zijn voor met als er maar om aan ook bij uit dan nog wat wordt door kan je ik we hij zij heeft zo ", U""},
      {"sv", " och i att det som en på är av för med till den inte har de om ett var jag hon men så han kan sig från vi du ", U"åäö"},
      {"da", " og i at det er en til på som de med han af for ikke der var den har jeg et sig men om så fra kan ", U"æøå"},
      {"no", " og i det er en til på som at de med han av for ikke der var den har jeg et seg men om så fra kan ", U"æøå"},
      {"pl", " i w na nie z do się że to jest jak o co a ale po od za przez dla tak jego jej już są był może tylko ", U"ąćęłńóśźż"},
      {"tr", " ve bir bu da de için ile ne çok daha gibi olarak olan ama ben sen o biz siz onlar var yok değil her ", U"çğışöü"},
      {"id", " dan yang di ini itu dengan untuk tidak dari dalam akan pada ke juga ada saya kami kita mereka atau oleh sudah bisa ", U""},
      {"vi", " và của là có không một những các cho được trong này với người đã để khi thì ", U"ăâđêôơưạảấầẩẫậắằẳẵặẹẻẽếềểễệỉịọỏốồổỗộớờởỡợụủứừửữựỳỵỷỹ"},
      {"cs", " a se na je v že to s z do o jako ale jsem jsou by pro tak po ", U"čďěňřšťůž"},
      {"ro", " și în de la cu un o că pe nu este sunt pentru mai din care ", U"ăâîșț"},
      {"hu", " a az és hogy nem is egy van de meg el ha ez azt mint csak ", U"őű"},
      {"fi", " ja on ei se että hän oli ovat kun mutta niin myös tai jos ", U"äö"},
  };
  const std::string low = " " + LowerUtf8Ascii(text) + " ";
  std::vector<std::string> words;
  {
    std::string w;
    for (char ch : low) {
      if (std::isalpha(static_cast<unsigned char>(ch)) || (static_cast<unsigned char>(ch) >= 0x80)) {
        w += ch;
      } else if (!w.empty()) {
        words.push_back(w);
        w.clear();
      }
    }
  }
  std::string best = "en";
  double best_score = -1;
  for (const auto& p : kProfiles) {
    double score = 0;
    const std::string list = p.words;
    for (const auto& w : words) {
      if (list.find(" " + w + " ") != std::string::npos) score += 1;
    }
    for (uint32_t c : cps) {
      if (!p.letters.empty() && p.letters.find(static_cast<char32_t>(c)) != std::u32string::npos) score += 0.6;
    }
    if (score > best_score) {
      best_score = score;
      best = p.code;
    }
  }
  return best;
}

// =DETECTLANGUAGE(text)
EV FDetectLanguage(Evaluator& ev, const Node& call) {
  YC_TEXT(text, 0);
  const std::string code = DetectLanguage(text);
  if (code.empty()) return Er(Err::Value);
  return EV::S(Value::String(code));
}

}  // namespace

void RegisterOnline(std::vector<FuncInfo>& t) {
  t.push_back({"STOCKHISTORY", 2, 11, FStockHistory});
  t.push_back({"TRANSLATE", 1, 3, FTranslate});
  t.push_back({"DETECTLANGUAGE", 1, 1, FDetectLanguage});
}

}  // namespace yc::formula::fn
