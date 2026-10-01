#include "config.h"
#include "core.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
void durationParsing() {
    using namespace lapse;
    std::wstring error;
    int64_t result = -7;
    struct Case { const wchar_t* text; DurationUnit unit; int64_t expected; };
    const Case cases[] = {{L"0.1",DurationUnit::Seconds,100},{L" \t1.234\r\n",DurationUnit::Seconds,1234},
        {L"1.5",DurationUnit::Minutes,90000},{L"0.000005",DurationUnit::Hours,18},
        {L"0.000000625",DurationUnit::Days,54},{L"1.000000000",DurationUnit::Days,86400000},
        {L"0001",DurationUnit::Seconds,1000}};
    for (const auto& item : cases) {
        check(parseDuration(item.text,item.unit,1,MaxCaptureIntervalMs,1,result,error) &&
              result == item.expected && error.empty(), "fixed decimal units convert exactly");
    }
    for (const auto* invalid : {L"",L" ",L"0",L"-1",L"+1",L".1",L"1.",L"1e3",L"nan",L"inf",L"1,5",
                               L"1 0",L"1..0",L"1.0000000000",L"0.1001",L"9999999999999999999999999999"}) {
        result = 73;
        check(!parseDuration(invalid,DurationUnit::Seconds,100,MaxCaptureIntervalMs,1,result,error) &&
              result == 73 && !error.empty(), "malformed, fractional millisecond and overflowing input cannot commit");
    }
    check(!parseDuration(L"86400.001",DurationUnit::Seconds,100,MaxCaptureIntervalMs,1,result,error), "capture maximum exact boundary");
    check(parseDuration(L"24",DurationUnit::Hours,100,MaxCaptureIntervalMs,1,result,error) && result==MaxCaptureIntervalMs,
          "capture maximum accepts another unit exactly");
    const int64_t maxStop = int64_t((std::numeric_limits<int>::max)()) * 1000;
    check(parseDuration(L"2147483647",DurationUnit::Seconds,1000,maxStop,1000,result,error) && result==maxStop,
          "stop duration supports full existing integer range");
    check(!parseDuration(L"2147483648",DurationUnit::Seconds,1000,maxStop,1000,result,error), "stop duration cannot overflow engine");
    check(!parseDuration(L"1.001",DurationUnit::Seconds,1000,maxStop,1000,result,error), "stop duration rejects partial seconds");
    check(parseDuration(L"0.05",DurationUnit::Minutes,1000,maxStop,1000,result,error) && result==3000,
          "stop duration decimal units can express whole seconds");
    check(!parseDuration(L"1",static_cast<DurationUnit>(77),100,maxStop,1,result,error), "unknown duration units rejected");
    check(!parseDuration(L"1",DurationUnit::Seconds,100,99,1,result,error) &&
          !parseDuration(L"1",DurationUnit::Seconds,-1,maxStop,1,result,error) &&
          !parseDuration(L"1",DurationUnit::Seconds,100,maxStop,0,result,error), "invalid parser contracts fail safely");
    check(parseDuration(L"0",DurationUnit::Seconds,0,maxStop,1000,result,error) && result==0,
          "recording offset permits exact zero");
    check(parseDuration(L"0.05",DurationUnit::Minutes,0,maxStop,1000,result,error) && result==3000,
          "zero-capable offset retains exact unit conversion");
    check(!parseDuration(L"-1",DurationUnit::Seconds,0,maxStop,1000,result,error) &&
          !parseDuration(L"0.001",DurationUnit::Seconds,0,maxStop,1000,result,error) &&
          !parseDuration(L"0",DurationUnit::Seconds,0,0,1000,result,error),
          "recording offset rejects negatives, partial seconds and invalid maximum");
    check(parseDuration(L"9223372036854775.807",DurationUnit::Seconds,1,(std::numeric_limits<int64_t>::max)(),1,result,error) &&
          result == (std::numeric_limits<int64_t>::max)(), "parser multiplication and fractional addition remain wide");
    check(!parseDuration(L"9223372036854775.808",DurationUnit::Seconds,1,(std::numeric_limits<int64_t>::max)(),1,result,error),
          "one-millisecond overflow is rejected");
}
void formatsAndBounds() {
    using namespace lapse;
    check(formatDuration(100)==L"0.1 seconds" && formatDuration(1000)==L"1 second" &&
          formatDuration(90000)==L"1.5 minutes" && formatDuration(86400000,true)==L"1 d", "exact readable duration labels");
    std::vector<int64_t> values{100,101,999,1000,1234,5000,90000,86400000,2147483647000};
    for (int i=0;i<2000;++i) values.push_back(100+int64_t(i)*43201);
    const std::pair<const wchar_t*,DurationUnit> units[]={{L" s",DurationUnit::Seconds},{L" min",DurationUnit::Minutes},
        {L" h",DurationUnit::Hours},{L" d",DurationUnit::Days}};
    std::wstring error;
    for (int64_t value : values) {
        const auto label = formatDuration(value,true);
        const auto split = label.find(L' ');
        bool passed = false;
        for (const auto& unit : units) if (label.substr(split)==unit.first) {
            int64_t parsed = 0;
            passed = parseDuration(label.substr(0,split),unit.second,1,(std::numeric_limits<int64_t>::max)(),1,parsed,error) && parsed==value;
        }
        check(passed, "every compact displayed value roundtrips without hidden rounding");
    }
    check(validateCaptureInterval(100,error) && validateCaptureInterval(86400000,error) &&
          !validateCaptureInterval(99,error) && !validateCaptureInterval(86400001,error), "shared interval bounds inclusive");
    int dimension = 72;
    for (const auto* invalid : {L"",L"47",L"49",L"4098",L"-48",L"48.0",L"4e2",L"48px",L"9999999999999999"})
        check(!parsePixelDimension(invalid,dimension,error) && dimension==72 && !error.empty(), "invalid pixel draft preserves committed value");
    check(parsePixelDimension(L" 4096 ",dimension,error) && dimension==4096, "maximum pixel dimension accepted");
    for (auto size : {std::pair<int,int>{48,48},{854,480},{1080,1920},{2160,4096},{4096,2160},{2974,2974}})
        check(validateVideoSize(size.first,size.second,error) && error.empty(), "valid custom geometry");
    for (auto size : {std::pair<int,int>{0,48},{48,47},{49,48},{4098,48},{4096,2162},{2976,2976},{INT_MAX,INT_MAX}})
        check(!validateVideoSize(size.first,size.second,error) && !error.empty(), "invalid geometry and area limit rejected safely");
}
void previewGeometry() {
    using namespace lapse;
    check(previewDimensions(1280,720)==std::pair<int,int>{640,360}, "default preview unchanged");
    check(previewDimensions(320,240)==std::pair<int,int>{480,360}, "4:3 preview fits source aspect");
    check(previewDimensions(1080,1920)==std::pair<int,int>{202,360}, "portrait preview remains small");
    check(previewDimensions(1080,1080)==std::pair<int,int>{360,360}, "square preview remains square");
    check(previewDimensions(0,0)==std::pair<int,int>{640,360} &&
          previewDimensions(INT_MAX,INT_MAX)==std::pair<int,int>{640,360}, "invalid preview selection has safe fallback");
    std::wstring error;
    for (int width=48;width<=4096;width+=22) for (int height=48;height<=4096;height+=26) {
        if (!validateVideoSize(width,height,error)) continue;
        const auto preview = previewDimensions(width,height);
        check(preview.first>=2 && preview.first<=640 && preview.second>=2 && preview.second<=360 &&
              preview.first%2==0 && preview.second%2==0, "preview allocation always bounded and even");
        const double scale = (std::min)(640.0/width,360.0/height);
        check(std::abs(preview.first-width*scale)<=1.000001 && std::abs(preview.second-height*scale)<=1.000001,
              "preview shape differs by at most one pixel on rounded shorter edge");
    }
}
void recordingFormats() {
    using namespace lapse;
    std::wstring error;
    for (auto mode : {EncodingMode::Compatible, EncodingMode::Efficient,
                      EncodingMode::HardwareH264, EncodingMode::QualityH264}) {
        check(validateEncodingMode(mode,false,error) && error.empty(), "ordinary H.264 remains valid");
        check(validateEncodingMode(mode,true,error) && error.empty(), "recovery supports each H.264 encoder");
    }
    check(validateEncodingMode(EncodingMode::HardwareHEVC,false,error) && error.empty(), "ordinary HEVC remains valid");
    check(!validateEncodingMode(EncodingMode::HardwareHEVC,true,error) && error.find(L"H.264")!=std::wstring::npos,
          "recovery rejects HEVC with an actionable message, without fallback");
    for (auto mode : {static_cast<EncodingMode>(-1),static_cast<EncodingMode>(5)})
        for (bool recovery : {false,true})
            check(!validateEncodingMode(mode,recovery,error) && !error.empty(), "invalid encoder rejected before file creation");
}
void playbackRates() {
    using namespace lapse;
    std::wstring error;
    int fps = 77;
    for (const auto* invalid : {L"",L" ",L"0",L"-1",L"+30",L"1.5",L"30.0",L"1e2",L"121",L"30 fps",L"1 0",L"9999999999999999"})
        check(!parseOutputFps(invalid,fps,error) && fps == 77 && !error.empty(),
              "invalid playback draft preserves the committed value");
    for (int value : {MinOutputFps,24,30,59,60,MaxOutputFps}) {
        check(parseOutputFps(L" \t" + std::to_wstring(value) + L"\r\n",fps,error) && fps == value && error.empty(),
              "supported whole playback FPS roundtrips exactly");
        check(validateOutputFps(value,error) && error.empty(), "shared playback bounds accept supported rates");
    }
    for (int value : {INT_MIN,-1,0,121,INT_MAX})
        check(!validateOutputFps(value,error) && !error.empty(), "shared playback bounds reject unsafe rates");
}
void sourceSizeSuggestions() {
    using lapse::sourceVideoDimensions;
    for (auto size : {std::pair<int,int>{1280,720},{1920,1080},{3440,1440},{3840,2160},{2160,3840},{48,48}})
        check(sourceVideoDimensions(size.first,size.second)==size,"supported source suggestions remain exact");
    check(sourceVideoDimensions(5120,1440)==std::pair<int,int>{4096,1152},"ultrawide screen fits without changing aspect");
    check(sourceVideoDimensions(1440,5120)==std::pair<int,int>{1152,4096},"portrait source preserves orientation");
    check(sourceVideoDimensions(641,481)==std::pair<int,int>{640,480} &&
          sourceVideoDimensions(48,49)==std::pair<int,int>{48,48},"odd source suggestions round down rather than upscale");
    check(sourceVideoDimensions(4096,4096)==std::pair<int,int>{2974,2974},"square source respects the total pixel budget");
    for (auto size : {std::pair<int64_t,int64_t>{0,0},{-1,720},{47,1080},{1080,47},
                     {INT_MAX,48},{48,INT_MAX},{int64_t(INT_MAX)+1,INT_MAX},{INT64_MAX,INT64_MAX}})
        check(sourceVideoDimensions(size.first,size.second)==std::pair<int,int>{0,0},"unusable or malformed sources have no suggestion");
    std::wstring error;
    for (int width : {48,49,100,641,1280,1919,3440,4096,5120,7680,16385,INT_MAX})
        for (int height : {48,49,240,481,720,1079,1440,2160,4096,4320,16385,INT_MAX}) {
            const auto size=sourceVideoDimensions(width,height);
            check(size==sourceVideoDimensions(width,height),"source suggestion is deterministic");
            if (!size.first) {check(!size.second,"unavailable suggestion has no partial geometry");continue;}
            check(lapse::validateVideoSize(size.first,size.second,error) && size.first<=width && size.second<=height,
                  "suggested sizes remain even, bounded and never upscaled");
            const double scale=(std::max)(double(size.first)/width,double(size.second)/height);
            check(width*scale-size.first<2.000001 && height*scale-size.second<2.000001,
                  "source aspect is preserved within even-pixel rounding");
        }
}
}
int main() {
    durationParsing(); formatsAndBounds(); previewGeometry(); recordingFormats(); playbackRates(); sourceSizeSuggestions();
    if (failures) return 1;
    std::cout << "Exact custom settings and bounded geometry checks passed\n";
    return 0;
}
