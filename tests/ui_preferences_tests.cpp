// Actual native preferences and real Win32 profile I/O in owned test directories.
// The window/control fixture stays hidden; no profile folders or sources are opened.
#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <new>

namespace preferenceAllocation {
thread_local bool failNext=false;
thread_local unsigned failures=0;
}
void* operator new(size_t size){
    if(preferenceAllocation::failNext){preferenceAllocation::failNext=false;++preferenceAllocation::failures;throw std::bad_alloc();}
    if(auto memory=std::malloc(size?size:1))return memory;
    throw std::bad_alloc();
}
void* operator new[](size_t size){return ::operator new(size);}
void operator delete(void* memory)noexcept{std::free(memory);}
void operator delete[](void* memory)noexcept{std::free(memory);}
void operator delete(void* memory,size_t)noexcept{std::free(memory);}
void operator delete[](void* memory,size_t)noexcept{std::free(memory);}

namespace lapse {
Settings configuredSettings, recordedSettings;
Status fixtureStatus;
unsigned recordCalls=0, configurationCalls=0;
std::function<void()> beforeRecording;
class FixtureEngine {
public:
    void configure(const Settings& value) {configuredSettings=value;++configurationCalls;}
    void refreshSources() {}
    void record() {if(beforeRecording)beforeRecording();recordedSettings=configuredSettings;++recordCalls;fixtureStatus.state=configuredSettings.startDelaySeconds?State::Waiting:State::Starting;}
    void pause() {}
    void setPaused(bool) {}
    void finish() {} void cancelDelayedStart() noexcept {} void setStatus(const StatusItem&) {}
    Status status(){return fixtureStatus;}
};
std::vector<Monitor> enumerateMonitors(){throw std::runtime_error("Unexpected device enumeration.");}
std::vector<CameraDevice> enumerateCameras(std::wstring&){throw std::runtime_error("Unexpected device enumeration.");}
int runCameraHost(const wchar_t*){throw std::runtime_error("Unexpected application entry.");}
}
namespace {
std::function<void()> beforePreferenceReplace;
int replacementAttempts=0;
DWORD replacementError=ERROR_SUCCESS;
enum class WriteFault { None, DenySecond, DenyCompression, DenySegment, DenyWatermark, DenyCursor, DenyNightDuration, DenyStartDelay, DenyOutputFps, DenyPauseHotkey, DenyStopHotkey, ThrowAfterFirst };
WriteFault writeFault=WriteFault::None;
int keyWrites=0, failedKeyWrites=0, syntheticExceptions=0, saveDiagnostics=0;
DWORD keyWriteError=ERROR_SUCCESS;std::wstring failedPreferenceKey;
HANDLE deniedWrite=INVALID_HANDLE_VALUE;
BOOL WINAPI fixtureWriteProfile(LPCWSTR section,LPCWSTR key,LPCWSTR value,LPCWSTR path){
    const BOOL result=WritePrivateProfileStringW(section,key,value,path);
    const DWORD error=result?ERROR_SUCCESS:GetLastError();
    if(section&&key){
        ++keyWrites;
        if(!result){++failedKeyWrites;keyWriteError=error;failedPreferenceKey=key;}
        const int denyAt=writeFault==WriteFault::DenySecond?2:writeFault==WriteFault::DenyCompression?25:writeFault==WriteFault::DenySegment?11:writeFault==WriteFault::DenyWatermark?32:writeFault==WriteFault::DenyCursor?14:writeFault==WriteFault::DenyNightDuration?16:writeFault==WriteFault::DenyStartDelay?37:writeFault==WriteFault::DenyOutputFps?38:writeFault==WriteFault::DenyPauseHotkey?39:writeFault==WriteFault::DenyStopHotkey?40:0;
        if(denyAt && keyWrites==denyAt-1 && result){
            deniedWrite=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(deniedWrite==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot deny the next owned staging write.");
        }else if(denyAt && keyWrites==denyAt && deniedWrite!=INVALID_HANDLE_VALUE){
            CloseHandle(deniedWrite);deniedWrite=INVALID_HANDLE_VALUE;
        }
        // This is a synthetic C++ exception to verify owned-stage cleanup;
        // ordinary Win32 profile API failures are tested above as BOOL/errors.
        if(writeFault==WriteFault::ThrowAfterFirst&&keyWrites==1&&result){
            ++syntheticExceptions;throw std::runtime_error("Synthetic exception after staged key write.");
        }
    }
    SetLastError(error);return result;
}
// Reset asks first; the answer and the question count are owned here.
int resetAnswer=IDNO,resetQuestions=0;
int WINAPI fixtureMessageBox(HWND,LPCWSTR,LPCWSTR title,UINT){
    if(std::wcscmp(title,L"Reset all settings")!=0)throw std::runtime_error("Unexpected message box.");
    ++resetQuestions;return resetAnswer;
}
void WINAPI fixtureSaveDebug(LPCWSTR message){
    if(std::wcscmp(message,L"Timelapse could not save preferences.\n")!=0)throw std::runtime_error("Unexpected preference diagnostic.");
    ++saveDiagnostics;
}
BOOL WINAPI fixtureMoveFileEx(LPCWSTR source,LPCWSTR target,DWORD flags){
    ++replacementAttempts;
    if(beforePreferenceReplace)beforePreferenceReplace();
    const BOOL result=MoveFileExW(source,target,flags);
    replacementError=result?ERROR_SUCCESS:GetLastError();return result;
}
}
#define Engine FixtureEngine
#define MoveFileExW fixtureMoveFileEx
#define WritePrivateProfileStringW fixtureWriteProfile
#define OutputDebugStringW fixtureSaveDebug
#define MessageBoxW fixtureMessageBox
// The reject-entry sentinel intentionally makes the GUI entry unreachable.
#pragma warning(push)
#pragma warning(disable: 4702)
#include "ui_person_pack_stub.h"
#include "../src/main.cpp"
#pragma warning(pop)
#undef Engine
#undef MoveFileExW
#undef WritePrivateProfileStringW
#undef OutputDebugStringW
#undef MessageBoxW

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct HiddenControls {
    HiddenControls(){
        app.window=CreateWindowExW(0,L"STATIC",L"Owned preferences fixture",WS_POPUP,0,0,500,400,nullptr,nullptr,nullptr,nullptr);
        require(app.window!=nullptr && !IsWindowVisible(app.window),"Cannot create hidden parent.");
        auto combo=[&](int count){
            auto window=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|CBS_DROPDOWNLIST,0,0,100,100,app.window,nullptr,nullptr,nullptr);
            require(window!=nullptr,"Cannot create owned option control.");
            for(int i=0;i<count;++i)add(window,std::to_wstring(i));choose(window,0);return window;
        };
        app.mode=combo(6);app.interval=combo(6);app.videoSize=combo(2);app.encodingQuality=combo(4);app.encodingMode=combo(static_cast<int>(std::size(EncodingModeLabels)));app.stopAfter=combo(6);
        app.splitEvery=combo(5);app.startDelay=combo(6);app.committedStartDelay=0;
        app.lowDisk=CreateWindowExW(0,L"BUTTON",L"Stop on low disk space",WS_CHILD|BS_AUTOCHECKBOX,0,0,200,30,app.window,nullptr,nullptr,nullptr);
        require(app.lowDisk!=nullptr,"Cannot create owned low disk option.");SendMessageW(app.lowDisk,BM_SETCHECK,BST_CHECKED,0);
        app.recoveryMode=CreateWindowExW(0,L"BUTTON",L"MP4 recovery mode (H.264)",WS_CHILD|BS_AUTOCHECKBOX,0,0,300,30,app.window,nullptr,nullptr,nullptr);
        require(app.recoveryMode!=nullptr,"Cannot create owned recovery option.");
        app.captureCursor=CreateWindowExW(0,L"BUTTON",L"Show desktop cursor",WS_CHILD|BS_AUTOCHECKBOX,0,0,240,30,app.window,nullptr,nullptr,nullptr);
        require(app.captureCursor!=nullptr,"Cannot create owned cursor option.");SendMessageW(app.captureCursor,BM_SETCHECK,BST_CHECKED,0);
        app.nightEnabled=CreateWindowExW(0,L"BUTTON",L"Night",WS_CHILD|BS_AUTOCHECKBOX,0,0,200,30,app.window,nullptr,nullptr,nullptr);
        require(app.nightEnabled!=nullptr,"Cannot create owned night option.");
        app.nightDuration=combo(6);app.nightTarget=combo(3);choose(app.nightTarget,1);
        for(HWND* target:{&app.alsoDesktop,&app.alsoCamera}){
            *target=CreateWindowExW(0,L"BUTTON",L"Companion",WS_CHILD|BS_AUTOCHECKBOX,0,0,200,30,app.window,nullptr,nullptr,nullptr);
            require(*target!=nullptr,"Cannot create owned companion option.");
        }
    }
    ~HiddenControls(){DestroyWindow(app.window);app.window=app.captureCursor=app.startDelay=app.alsoDesktop=app.alsoCamera=nullptr;}
};
struct PreferencesFixture {
    std::filesystem::path base=std::filesystem::current_path(), directory, settingsDirectory;
    std::vector<std::filesystem::path> ownedDirectories;
    std::wstring priorPreferences=app.preferences, priorFolder=app.settings.folder;
    explicit PreferencesFixture(size_t pathLength=0){
        static unsigned counter=0;
        directory=base/(L"ui-preferences-"+std::to_wstring(GetCurrentProcessId())+L"-"+
                       std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++counter));
        require(base.is_absolute() && directory.parent_path()==base,"Preferences fixture must be an absolute owned child.");
        require(std::filesystem::create_directory(directory),"Cannot create unique owned preferences directory.");
        ownedDirectories.push_back(directory);settingsDirectory=directory;
        if(pathLength){
            const size_t parentLength=pathLength-std::wstring(L"\\settings.ini").size();
            require(settingsDirectory.wstring().size()+2<=parentLength,"Owned test prefix is too long for the settings boundary.");
            while(settingsDirectory.wstring().size()<parentLength){
                const size_t remaining=parentLength-settingsDirectory.wstring().size();
                require(remaining>=2,"Cannot construct the exact owned path length.");
                size_t component=std::min<size_t>(40,remaining-1);
                if(remaining-component-1==1)--component;
                settingsDirectory/=std::wstring(component,L'p');
                require(CreateDirectoryW(fileIOPath(settingsDirectory.wstring()).c_str(),nullptr)!=FALSE,"Cannot create an owned nested directory.");
                ownedDirectories.push_back(settingsDirectory);
            }
        }
        app.preferences=(settingsDirectory/L"settings.ini").wstring();
        require(std::filesystem::path(app.preferences).parent_path()==settingsDirectory,"INI escaped its owned directory.");
        require(!pathLength||app.preferences.size()==pathLength,"Wrong exact settings path length.");
        replacementAttempts=0;replacementError=ERROR_SUCCESS;beforePreferenceReplace=nullptr;
        writeFault=WriteFault::None;keyWrites=failedKeyWrites=syntheticExceptions=saveDiagnostics=0;keyWriteError=ERROR_SUCCESS;
    }
    ~PreferencesFixture(){
        preferenceAllocation::failNext=false;writeFault=WriteFault::None;
        if(deniedWrite!=INVALID_HANDLE_VALUE){CloseHandle(deniedWrite);deniedWrite=INVALID_HANDLE_VALUE;}
        beforePreferenceReplace=nullptr;
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,fileIOPath(app.preferences).c_str());
        app.preferences=priorPreferences;app.settings.folder=priorFolder;
        // Only traverse flat files in directories created by this instance,
        // deepest first; reject reparse points and any unexpected subdirectory.
        std::error_code error;
        if(directory.is_absolute() && directory.parent_path()==base){
            const auto prefix=directory.wstring()+L"\\";
            for(auto it=ownedDirectories.rbegin();it!=ownedDirectories.rend()&&!error;++it){
                const auto name=it->wstring();
                if(*it!=directory&&name.compare(0,prefix.size(),prefix)!=0)break;
                const std::filesystem::path io(fileIOPath(name));
                const DWORD attributes=GetFileAttributesW(io.c_str());
                if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))break;
                for(std::filesystem::directory_iterator file(io,error),end;!error&&file!=end;file.increment(error)){
                    const DWORD childAttributes=GetFileAttributesW(file->path().c_str());
                    if(file->path().parent_path()!=io||childAttributes==INVALID_FILE_ATTRIBUTES||
                       (childAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){
                        error=std::make_error_code(std::errc::permission_denied);break;
                    }
                    std::filesystem::remove(file->path(),error);
                }
                if(!error)std::filesystem::remove(io,error);
            }
        }
    }
    std::string bytes() const {
        std::ifstream input(std::filesystem::path(fileIOPath(app.preferences)),std::ios::binary);
        require(bool(input),"Cannot read owned INI bytes.");
        std::string value(std::istreambuf_iterator<char>(input),{});
        require(!input.bad(),"Cannot read complete owned INI.");return value;
    }
    void seed(const std::string& value) const {
        std::ofstream output(std::filesystem::path(fileIOPath(app.preferences)),std::ios::binary);
        output.write(value.data(),static_cast<std::streamsize>(value.size()));output.close();
        require(bool(output),"Cannot seed owned INI.");
    }
    void onlySettingsRemain() const {
        size_t count=0;
        for(const auto& entry:std::filesystem::directory_iterator(std::filesystem::path(fileIOPath(settingsDirectory.wstring())))){
            require(entry.path().filename()==L"settings.ini","A preference save left a temporary file.");++count;
        }
        require(count==1,"Settings file was removed or an unexpected file was left.");
    }
};
const std::string legacy="; keep this comment\r\n[Settings]\r\nFolder=C:\\Prior\r\nInterval=4\r\nQuality=1\r\nEncodingQuality=0\r\nFuture=preserved\r\n[Another]\r\nKey=unchanged\r\n";
const std::wstring unicodeFolder=L"C:\\Synthetic videos\\\u65e5\u672c\u8a9e-\U0001f4f7";
std::string utf16(const std::wstring& text){const std::wstring value=L"\ufeff"+text;return {reinterpret_cast<const char*>(value.data()),value.size()*sizeof(wchar_t)};}
void expectOptions(const std::wstring& folder,int interval,int size,int encoding){
    require(app.settings.folder==folder,"Unicode save folder changed on reload.");
    require(choice(app.interval)==interval && choice(app.videoSize)==size && choice(app.encodingQuality)==encodingQualityChoice(static_cast<EncodingQuality>(encoding)),
            "Persisted Interval/Quality/EncodingQuality changed.");
    require(choice(app.mode)==0,"Preferences load did not preserve Desktop startup mode.");
}
void reload(){
    app.settings.folder=L"C:\\Default";choose(app.interval,0);choose(app.videoSize,0);choose(app.encodingQuality,encodingQualityChoice(static_cast<EncodingQuality>(1)));choose(app.mode,4);
    preferences(false);
}
void expectUtf16(const PreferencesFixture& fixture){const auto bytes=fixture.bytes();require(bytes.size()>=2 && static_cast<unsigned char>(bytes[0])==0xff && static_cast<unsigned char>(bytes[1])==0xfe,"Saved preferences lack the Unicode BOM.");}
void expectUnknownContent(const PreferencesFixture& fixture){
    const auto io=fileIOPath(app.preferences);
    wchar_t value[64]{};
    GetPrivateProfileStringW(L"Settings",L"Future",L"",value,64,io.c_str());require(std::wstring(value)==L"preserved","Unrelated key was lost.");
    GetPrivateProfileStringW(L"Another",L"Key",L"",value,64,io.c_str());require(std::wstring(value)==L"unchanged","Unrelated section was lost.");
    const std::wstring comment=L"; keep this comment";
    require(fixture.bytes().find(std::string(reinterpret_cast<const char*>(comment.data()),comment.size()*sizeof(wchar_t)))!=std::string::npos,
            "An existing comment was lost during migration.");
}
void newUnicodeFile(){
    PreferencesFixture fixture;
    app.settings.folder=unicodeFolder;choose(app.interval,5);choose(app.videoSize,1);choose(app.encodingQuality,encodingQualityChoice(static_cast<EncodingQuality>(2)));
    preferences(true);reload();expectOptions(unicodeFolder,5,1,2);expectUtf16(fixture);fixture.onlySettingsRemain();
    std::cout<<"PASS new Unicode folder and all numeric options roundtrip through real profile APIs\n";
}
void existingUnicodeRewrite(){
    PreferencesFixture fixture;
    const std::wstring initial=L"[Settings]\r\nFolder="+unicodeFolder+L"\r\nInterval=5\r\nQuality=1\r\nEncodingQuality=2\r\nAv1Preset=6\r\nEncodingRateControl=0\r\nAv1Crf=32\r\nEncodingBitrateKbps=4000\r\nEncodingMode=0\r\nRecordingLimit=0\r\nStopOnLowDiskSpace=1\r\nRecoveryMode=0\r\nShowDesktopCursor=1\r\nNightEnabled=0\r\nNightDurationMs=0\r\nNightTargetBrightness=96\r\nCaptureIntervalMs=5000\r\nVideoWidth=1280\r\nVideoHeight=720\r\nRecordingLimitSeconds=900\r\nSegmentDurationSeconds=0\r\nTimeSkipMode=0\r\nTimeSkipMultiplier=4\r\nTimeSkipQuietAfterMs=120000\r\nTimeSkipRampFrames=30\r\nTimeSkipRepeatSeconds=0\r\nTimeSkipRanges=\r\nTimeSkipQuietSensitivity=1\r\nTimeSkipUncertainAsAbsent=1\r\nWatermarkEnabled=0\r\nWatermarkShowTime=1\r\nWatermarkShowSpeed=1\r\nWatermarkTimeKind=0\r\nWatermarkX=10000\r\nWatermarkY=10000\r\nWatermarkTextSize=1\r\nStartDelaySeconds=0\r\nOutputFps=30\r\nPauseHotkey=0\r\nStopHotkey=0\r\nAlsoSaveDesktop=0\r\nAlsoSaveCamera=0\r\nStatusHotkey=0\r\nStatusCorner=0\r\nStatusTextSize=1\r\nStatusStyle=0\r\nStatusLastKind=0\r\nStatusTimerSeconds=1500\r\nStatusBreakSeconds=300\r\nStatusRepeat=0\r\nStatusSaveLog=0\r\n";
    fixture.seed(utf16(initial));reload();expectOptions(unicodeFolder,5,1,2);
    const auto initialBytes=fixture.bytes();
    const std::wstring changed=L"C:\\Synthetic videos\\\u65e5\u672c\u8a9e-\U0001f3a5";
    require(changed.size()==unicodeFolder.size(),"Rewrite must keep the same Unicode string length.");
    app.settings.folder=changed;choose(app.interval,1);choose(app.videoSize,0);choose(app.encodingQuality,encodingQualityChoice(static_cast<EncodingQuality>(0)));
    preferences(true);reload();expectOptions(changed,1,0,0);expectUtf16(fixture);
    require(fixture.bytes().size()==initialBytes.size(),"Rewrite did not retain the same file size for the cache regression.");
    fixture.onlySettingsRemain();std::cout<<"PASS same-length existing UTF16 rewrite does not reload cached old settings\n";
}
void migrateAnsi(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();expectOptions(L"C:\\Prior",4,1,0);
    app.settings.folder=unicodeFolder;preferences(true);reload();expectOptions(unicodeFolder,4,1,0);
    expectUtf16(fixture);expectUnknownContent(fixture);fixture.onlySettingsRemain();
    std::cout<<"PASS ANSI migration preserves options, comments, unknown key and unrelated section\n";
}
void encodingModes(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();
    require(choice(app.encodingMode)==0,"Old preferences changed the encoder without an explicit selection.");
    const int modes=static_cast<int>(std::size(EncodingModeLabels));
    require(modes==6,"Encoding mode list does not match the saved mode values.");
    for(int mode=0;mode<modes;++mode){
        choose(app.encodingMode,mode);preferences(true);choose(app.encodingMode,(mode+1)%modes);reload();
        require(choice(app.encodingMode)==mode,"Encoding mode did not survive a real preference roundtrip.");
    }
    for(const wchar_t* invalid:{L"-1",L"6",L"999"}){
        require(WritePrivateProfileStringW(L"Settings",L"EncodingMode",invalid,app.preferences.c_str())!=FALSE,"Cannot seed invalid mode.");
        reload();require(choice(app.encodingMode)==0,"Invalid encoder setting selected a different codec.");
    }
    fixture.onlySettingsRemain();
    std::cout<<"PASS encoding modes roundtrip; old or invalid settings preserve compatible H.264\n";
}
void recordingLimits(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();
    require(choice(app.stopAfter)==0,"Old preferences enabled automatic stop without an explicit choice.");
    for(int limit=0;limit<6;++limit){
        choose(app.stopAfter,limit);preferences(true);choose(app.stopAfter,(limit+1)%6);reload();
        require(choice(app.stopAfter)==limit,"Recording time limit did not survive atomic preference roundtrip.");
    }
    for(const wchar_t* invalid:{L"-1",L"7",L"999"}){
        require(WritePrivateProfileStringW(L"Settings",L"RecordingLimit",invalid,app.preferences.c_str())!=FALSE,"Cannot seed invalid recording limit.");
        reload();require(choice(app.stopAfter)==0,"Invalid recording limit enabled automatic stop.");
    }
    fixture.onlySettingsRemain();std::cout<<"PASS recording limits roundtrip; old or invalid settings remain unlimited\n";
}
void startDelayOptions(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();configure();
    const auto records=lapse::recordCalls;
    require(choice(app.startDelay)==0 && !app.settings.startDelaySeconds,"Legacy preferences enabled a self-timer.");
    for(int index=0;index<6;++index){
        choose(app.startDelay,index);configure();preferences(true);choose(app.startDelay,(index+1)%6);reload();configure();
        wchar_t value[32]{};GetPrivateProfileStringW(L"Settings",L"StartDelaySeconds",L"",value,32,app.preferences.c_str());
        require(choice(app.startDelay)==index && app.settings.startDelaySeconds==StartDelays[index] && std::wstring(value)==std::to_wstring(StartDelays[index]) && choice(app.mode)==0,
            "Self-timer preset lost exact seconds or Desktop startup.");
    }
    for(const wchar_t* invalid:{L"",L"-1",L"1",L"4",L"301",L"5junk",L"05",L"+5",L"1.5",L"999999999999999999999999999999999999999"}){
        require(WritePrivateProfileStringW(L"Settings",L"StartDelaySeconds",invalid,app.preferences.c_str())!=FALSE,"Cannot seed invalid delay.");
        reload();configure();require(choice(app.startDelay)==0 && !app.settings.startDelaySeconds,"Malformed saved delay armed a timer.");
    }
    require(WritePrivateProfileStringW(L"Settings",L"StartDelaySeconds",nullptr,app.preferences.c_str())!=FALSE,"Cannot remove optional delay.");
    reload();configure();require(!app.settings.startDelaySeconds,"Absent delay did not default to None.");
    choose(app.startDelay,3);configure();preferences(true);const auto previous=fixture.bytes();
    choose(app.startDelay,5);configure();keyWrites=failedKeyWrites=0;failedPreferenceKey.clear();writeFault=WriteFault::DenyStartDelay;preferences(true);writeFault=WriteFault::None;
    require(keyWrites==37 && failedKeyWrites==1 && failedPreferenceKey==L"StartDelaySeconds" && fixture.bytes()==previous,"Failed final delay key published partial preferences.");
    reload();configure();require(app.settings.startDelaySeconds==30 && lapse::recordCalls==records,"Preference loading armed a start or lost the prior delay after a failed save.");
    expectUnknownContent(fixture);fixture.onlySettingsRemain();
    std::cout<<"PASS self-timer exact presets/defaults/strict fallback, no armed-state persistence and final-key atomic failure\n";
}
void playbackStagingRollback(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();preferences(true);
    const auto previous=fixture.bytes();
    const WriteFault faults[]={WriteFault::DenyOutputFps,WriteFault::DenyPauseHotkey,WriteFault::DenyStopHotkey};
    constexpr const wchar_t* keys[]={L"OutputFps",L"PauseHotkey",L"StopHotkey"};
    for(int i=0;i<3;++i){
        app.settings.outputFps=60;app.pauseHotkey=static_cast<uint16_t>('P'|((HOTKEYF_CONTROL|HOTKEYF_ALT)<<8));app.stopHotkey=static_cast<uint16_t>('S'|((HOTKEYF_CONTROL|HOTKEYF_ALT)<<8));
        keyWrites=failedKeyWrites=0;failedPreferenceKey.clear();writeFault=faults[i];preferences(true);writeFault=WriteFault::None;
        require(keyWrites==38+i && failedKeyWrites==1 && failedPreferenceKey==keys[i] && fixture.bytes()==previous,"Failed playback/shortcut staging key replaced prior preferences.");
        reload();require(app.settings.outputFps==30 && !app.pauseHotkey && !app.stopHotkey,"Playback staging failure published partial new values.");
        fixture.onlySettingsRemain();
    }
    std::cout<<"PASS atomic rollback at each playback/final shortcut key preserves the prior complete INI\n";
}
void segmentOptions(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();configure();
    require(!app.settings.segmentDurationSeconds && choice(app.splitEvery)==0,"Old preferences enabled splitting.");
    for(int seconds:{0,900,3600,21600,86400,1,90,2147483647}){
        app.customSegmentSeconds=seconds;app.hasCustomSegment=true;app.committedSegment=5;normalizeCustomSelections();customItems();configure();preferences(true);
        choose(app.splitEvery,0);reload();configure();
        wchar_t value[48]{};GetPrivateProfileStringW(L"Settings",L"SegmentDurationSeconds",L"",value,48,app.preferences.c_str());
        require(app.settings.segmentDurationSeconds==seconds && std::wstring(value)==std::to_wstring(seconds) && choice(app.mode)==0,
            "Split seconds lost exact atomic roundtrip or Desktop startup.");
        const bool preset=std::find(std::begin(SegmentDurations),std::end(SegmentDurations),seconds)!=std::end(SegmentDurations);
        require(app.hasCustomSegment!=preset && SendMessageW(app.splitEvery,CB_GETCOUNT,0,0)==(preset?6:7),"Split load duplicated a preset or lost its permanent Custom action.");
    }
    for(const wchar_t* invalid:{L"",L"-1",L"1.5",L"1junk",L"01",L"2147483648",L"999999999999999999999999999999999999999999999999999999999"}){
        require(WritePrivateProfileStringW(L"Settings",L"SegmentDurationSeconds",invalid,app.preferences.c_str()),"Cannot seed malformed splitting preference.");reload();configure();
        require(!app.settings.segmentDurationSeconds && !app.hasCustomSegment && choice(app.splitEvery)==0,"Malformed saved split enabled partial/stale settings.");
    }
    require(WritePrivateProfileStringW(L"Settings",L"SegmentDurationSeconds",L"90",app.preferences.c_str()),"Cannot seed prior split.");reload();configure();
    const auto previous=fixture.bytes();choose(app.splitEvery,2);configure();keyWrites=failedKeyWrites=0;writeFault=WriteFault::DenySegment;preferences(true);writeFault=WriteFault::None;
    require(keyWrites==11 && failedKeyWrites==1 && fixture.bytes()==previous,"Failed split-key write published a partial settings file.");reload();configure();
    require(app.settings.segmentDurationSeconds==90,"Failed transaction lost the prior exact split setting.");
    expectUnknownContent(fixture);fixture.onlySettingsRemain();
    std::cout<<"PASS exact split presets/custom/max/off preferences, strict malformed fallback, preset normalization and atomic split-key failure\n";
}
void companionOptions(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();
    require(!isChecked(app.alsoDesktop)&&!isChecked(app.alsoCamera),"Old preferences enabled companion files.");
    for(bool desktop:{true,false})for(bool camera:{true,false}){
        SendMessageW(app.alsoDesktop,BM_SETCHECK,desktop?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(app.alsoCamera,BM_SETCHECK,camera?BST_CHECKED:BST_UNCHECKED,0);preferences(true);
        SendMessageW(app.alsoDesktop,BM_SETCHECK,desktop?BST_UNCHECKED:BST_CHECKED,0);
        SendMessageW(app.alsoCamera,BM_SETCHECK,camera?BST_UNCHECKED:BST_CHECKED,0);reload();
        require(isChecked(app.alsoDesktop)==desktop&&isChecked(app.alsoCamera)==camera&&choice(app.mode)==0,
            "Companion choices lost their roundtrip or restored a collage at startup.");
    }
    for(const wchar_t* invalid:{L"",L"2",L"true",L"01",L"1junk"}){
        for(const wchar_t* key:{L"AlsoSaveDesktop",L"AlsoSaveCamera"})
            require(WritePrivateProfileStringW(L"Settings",key,invalid,app.preferences.c_str())!=FALSE,"Cannot seed invalid companion option.");
        reload();require(!isChecked(app.alsoDesktop)&&!isChecked(app.alsoCamera),"Malformed companion option enabled extra files.");
    }
    fixture.onlySettingsRemain();std::cout<<"PASS companion file choices roundtrip; old and malformed settings keep them off\n";
}
void diskSafety(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();
    require(SendMessageW(app.lowDisk,BM_GETCHECK,0,0)==BST_CHECKED,"Old preferences disabled low disk protection.");
    for(int checked:{BST_UNCHECKED,BST_CHECKED}){
        SendMessageW(app.lowDisk,BM_SETCHECK,checked,0);preferences(true);
        SendMessageW(app.lowDisk,BM_SETCHECK,checked==BST_CHECKED?BST_UNCHECKED:BST_CHECKED,0);reload();
        require(SendMessageW(app.lowDisk,BM_GETCHECK,0,0)==checked,"Low disk protection lost its persisted selection.");
    }
    for(const wchar_t* invalid:{L"",L"-1",L"2",L"999",L"false",L"0junk",L"00000000000000000000000"}){
        require(WritePrivateProfileStringW(L"Settings",L"StopOnLowDiskSpace",invalid,app.preferences.c_str())!=FALSE,"Cannot seed invalid low disk option.");
        reload();require(SendMessageW(app.lowDisk,BM_GETCHECK,0,0)==BST_CHECKED,"Invalid low disk option disabled protection.");
    }
    fixture.onlySettingsRemain();std::cout<<"PASS low disk protection roundtrip; old and malformed settings keep protection enabled\n";
}
void cursorOptions(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();configure();
    require(app.settings.captureCursor&&SendMessageW(app.captureCursor,BM_GETCHECK,0,0)==BST_CHECKED,"Legacy preferences disabled the cursor.");
    for(bool enabled:{false,true}){
        SendMessageW(app.captureCursor,BM_SETCHECK,enabled?BST_CHECKED:BST_UNCHECKED,0);preferences(true);
        wchar_t stored[8]{};GetPrivateProfileStringW(L"Settings",L"ShowDesktopCursor",L"missing",stored,8,app.preferences.c_str());
        require(std::wstring(stored)==(enabled?L"1":L"0"),"Cursor preference was not serialized exactly.");
        reload();app.settings.layers=preset(Mode::Camera);configure();require(app.settings.captureCursor==enabled,"Cursor roundtrip/source change lost the retained choice.");
    }
    for(const wchar_t* invalid:{L"",L"?",L"2",L"00",L"0junk",L"-1",L"0000000000000000000000000000000000000"}){
        require(WritePrivateProfileStringW(L"Settings",L"ShowDesktopCursor",invalid,app.preferences.c_str())!=FALSE,"Cannot seed malformed cursor preference.");
        reload();configure();require(app.settings.captureCursor,"Malformed cursor preference silently disabled the cursor.");
    }
    WritePrivateProfileStringW(L"Settings",L"ShowDesktopCursor",L"1",app.preferences.c_str());const auto before=fixture.bytes();
    SendMessageW(app.captureCursor,BM_SETCHECK,BST_UNCHECKED,0);keyWrites=failedKeyWrites=0;failedPreferenceKey.clear();writeFault=WriteFault::DenyCursor;preferences(true);writeFault=WriteFault::None;
    require(keyWrites==14&&failedKeyWrites==1&&failedPreferenceKey==L"ShowDesktopCursor"&&fixture.bytes()==before,"Failed cursor staging write replaced old settings.");
    reload();configure();require(app.settings.captureCursor,"Failed cursor save changed the persisted default.");expectUnknownContent(fixture);fixture.onlySettingsRemain();
    std::cout<<"PASS cursor exact/default persistence, camera-only retention, malformed rejection and atomic named-key failure\n";
}
void recoveryOptions(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();configure();
    require(!app.settings.recoveryMode&&SendMessageW(app.recoveryMode,BM_GETCHECK,0,0)==BST_UNCHECKED,"Legacy preferences enabled recovery without consent.");
    for(int checked:{BST_CHECKED,BST_UNCHECKED}){
        SendMessageW(app.recoveryMode,BM_SETCHECK,checked,0);preferences(true);
        SendMessageW(app.recoveryMode,BM_SETCHECK,checked==BST_CHECKED?BST_UNCHECKED:BST_CHECKED,0);reload();configure();
        require(app.settings.recoveryMode==(checked==BST_CHECKED)&&SendMessageW(app.recoveryMode,BM_GETCHECK,0,0)==checked,"Recovery selection lost its atomic roundtrip.");
    }
    for(const wchar_t* invalid:{L"",L"-1",L"2",L"true",L"01",L"1junk",L"111111111111111111111111111111111111"}){
        require(WritePrivateProfileStringW(L"Settings",L"RecoveryMode",invalid,app.preferences.c_str())!=FALSE,"Cannot seed malformed recovery mode.");
        reload();configure();require(!app.settings.recoveryMode,"Malformed recovery preference opted in.");
    }
    SendMessageW(app.recoveryMode,BM_SETCHECK,BST_CHECKED,0);choose(app.encodingMode,3);preferences(true);reload();configure();
    require(app.settings.recoveryMode&&app.settings.encodingMode==EncodingMode::HardwareHEVC&&!app.encodingValidation.empty(),"Reload silently changed the persisted HEVC/recovery choice instead of validation.");
    expectUnknownContent(fixture);fixture.onlySettingsRemain();
    SendMessageW(app.recoveryMode,BM_SETCHECK,BST_UNCHECKED,0);choose(app.encodingMode,0);configure();
    std::cout<<"PASS recovery strict opt-in/default, atomic roundtrip, malformed rejection and retained visible HEVC incompatibility\n";
}
void customOptions(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();
    for(int interval:{100,125,1234567,86400000}){
        app.customIntervalMs=interval;app.customWidth=1080;app.customHeight=1920;app.customLimitSeconds=INT_MAX;
        app.hasCustomInterval=app.hasCustomSize=app.hasCustomLimit=true;
        app.committedInterval=6;app.committedSize=2;app.committedLimit=6;customItems();preferences(true);reload();configure();
        require(app.settings.intervalMs==interval && app.settings.width==1080 && app.settings.height==1920 && app.settings.recordingLimitSeconds==INT_MAX,
            "Exact custom values failed their atomic preference roundtrip.");
        require(choice(app.interval)==6 && choice(app.videoSize)==2 && choice(app.stopAfter)==6 && choice(app.mode)==0,"Custom selectors or Desktop startup were lost.");
    }
    choose(app.interval,1);choose(app.videoSize,0);choose(app.stopAfter,0);preferences(true);reload();configure();
    require(app.settings.intervalMs==2000 && app.settings.width==1280 && app.settings.height==720 && !app.settings.recordingLimitSeconds,
        "Stale custom keys overrode a newer preset selection.");
    const auto seedCustom=[&](const wchar_t* interval,const wchar_t* width,const wchar_t* height,const wchar_t* stop){
        const std::wstring settings=L"[Settings]\r\nInterval=6\r\nQuality=2\r\nRecordingLimit=6\r\nCaptureIntervalMs="+std::wstring(interval)+
            L"\r\nVideoWidth="+width+L"\r\nVideoHeight="+height+L"\r\nRecordingLimitSeconds="+stop+L"\r\n";
        fixture.seed(utf16(settings));reload();configure();
    };
    for(const wchar_t* invalid:{L"",L"-1",L"125junk",L"1.5",L"99999999999999999999999999999999999999999999999999999999"}){
        seedCustom(invalid,invalid,invalid,invalid);
        require(app.settings.intervalMs==5000 && app.settings.width==1280 && app.settings.height==720 && !app.settings.recordingLimitSeconds,
            "Malformed custom values enabled a partial or unintended policy.");
    }
    seedCustom(L"99",L"49",L"720",L"0");require(app.settings.intervalMs==5000 && app.settings.width==1280 && !app.settings.recordingLimitSeconds,"Out-of-range custom values were admitted.");
    seedCustom(L"86400001",L"4096",L"4096",L"2147483648");require(app.settings.intervalMs==5000 && app.settings.width==1280 && !app.settings.recordingLimitSeconds,"Custom area/integer bounds overflowed.");
    seedCustom(L"2000",L"1280",L"720",L"900");
    require(choice(app.interval)==1 && choice(app.videoSize)==0 && choice(app.stopAfter)==1 && !app.hasCustomInterval && !app.hasCustomSize && !app.hasCustomLimit,
        "Saved custom values equal to presets retained duplicate custom rows.");
    for(const auto& suffix:{std::wstring(L"junk"),std::wstring(L".5"),std::wstring(60,L' ')+L"junk"}){
        const std::wstring settings=L"[Settings]\r\nInterval=6"+suffix+L"\r\nQuality=2"+suffix+L"\r\nRecordingLimit=6"+suffix+
            L"\r\nCaptureIntervalMs=125\r\nVideoWidth=1080\r\nVideoHeight=1920\r\nRecordingLimitSeconds=90\r\n";
        fixture.seed(utf16(settings));reload();configure();
        require(!app.hasCustomInterval && !app.hasCustomSize && !app.hasCustomLimit && app.settings.intervalMs==5000 && app.settings.width==1280 &&
            app.settings.height==720 && !app.settings.recordingLimitSeconds,"A prefix-parsed legacy selector activated exact custom settings.");
    }
    fixture.onlySettingsRemain();std::cout<<"PASS exact custom interval/size/stop roundtrips, malformed fallback and newer preset precedence\n";
}
void nightOptions(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();
    require(SendMessageW(app.nightEnabled,BM_GETCHECK,0,0)==BST_UNCHECKED&&choice(app.nightDuration)==0&&choice(app.nightTarget)==1,"Old preferences enabled night capture or changed Auto/Balanced defaults.");
    SendMessageW(app.nightEnabled,BM_SETCHECK,BST_CHECKED,0);
    for(int duration=0;duration<6;++duration)for(int target=0;target<3;++target){
        choose(app.nightDuration,duration);choose(app.nightTarget,target);preferences(true);
        SendMessageW(app.nightEnabled,BM_SETCHECK,BST_UNCHECKED,0);choose(app.nightDuration,0);choose(app.nightTarget,1);reload();
        require(SendMessageW(app.nightEnabled,BM_GETCHECK,0,0)==BST_CHECKED&&choice(app.nightDuration)==duration&&choice(app.nightTarget)==target&&choice(app.mode)==0,"Night settings roundtrip lost an exact value or changed Desktop startup.");
        require(GetPrivateProfileIntW(L"Settings",L"NightDurationMs",-1,app.preferences.c_str())==static_cast<UINT>(NightDurations[duration])&&GetPrivateProfileIntW(L"Settings",L"NightTargetBrightness",-1,app.preferences.c_str())==static_cast<UINT>(NightTargets[target]),"Night settings persisted indexes instead of policy values.");
    }
    for(const auto key:{L"NightEnabled",L"NightDurationMs",L"NightTargetBrightness"})for(const wchar_t* invalid:{L"",L"-1",L"2junk",L"999999999999999999999999999999999999999"}){
        require(WritePrivateProfileStringW(L"Settings",key,invalid,app.preferences.c_str())!=FALSE,"Cannot seed invalid night option.");reload();
        if(std::wcscmp(key,L"NightEnabled")==0)require(SendMessageW(app.nightEnabled,BM_GETCHECK,0,0)==BST_UNCHECKED,"Malformed night flag enabled capture.");
        else require(choice(std::wcscmp(key,L"NightDurationMs")==0?app.nightDuration:app.nightTarget)==(std::wcscmp(key,L"NightDurationMs")==0?0:1),"Malformed night policy did not restore Auto/Balanced default.");
    }
    for(int duration:{1000,1001,1501,2000,3000,5000,10000,15000,29999,30000}){
        require(WritePrivateProfileStringW(L"Settings",L"NightDurationMs",std::to_wstring(duration).c_str(),app.preferences.c_str())!=FALSE,"Cannot seed exact custom Night duration.");
        reload();configure();int preset=-1;for(int i=1;i<6;++i)if(NightDurations[i]==duration)preset=i;
        require(selectedNightDuration()==duration && app.settings.night.durationMs==duration && choice(app.nightDuration)==(preset<0?6:preset) && app.hasCustomNightDuration==(preset<0),
            "Night duration load rounded milliseconds or failed preset normalization.");
        preferences(true);choose(app.nightDuration,0);reload();configure();
        wchar_t stored[32]{};GetPrivateProfileStringW(L"Settings",L"NightDurationMs",L"missing",stored,32,app.preferences.c_str());
        require(std::wstring(stored)==std::to_wstring(duration) && selectedNightDuration()==duration && app.settings.night.durationMs==duration && choice(app.mode)==0,
            "Custom Night duration did not survive exact-value persistence or enabled a camera on startup.");
    }
    for(const wchar_t* invalid:{L"999",L"30001",L"1.5",L"1500junk",L"+1500",L"-1500",L"2147483648",L"1500.0",L"1500e0"}){
        require(WritePrivateProfileStringW(L"Settings",L"NightDurationMs",invalid,app.preferences.c_str())!=FALSE,"Cannot seed invalid custom Night duration.");reload();configure();
        require(selectedNightDuration()==0 && app.settings.night.durationMs==0 && choice(app.nightDuration)==0 && !app.hasCustomNightDuration && SendMessageW(app.nightDuration,CB_GETCOUNT,0,0)==7,
            "Invalid custom Night persistence enabled a prefix, retained old custom state or lost Auto.");
    }
    require(WritePrivateProfileStringW(L"Settings",L"NightDurationMs",L"1501",app.preferences.c_str())!=FALSE,"Cannot seed custom Night rollback baseline.");reload();
    const auto before=fixture.bytes();app.customNightDurationMs=15000;app.hasCustomNightDuration=true;app.committedNightDuration=6;customItems();
    keyWrites=failedKeyWrites=0;failedPreferenceKey.clear();writeFault=WriteFault::DenyNightDuration;preferences(true);writeFault=WriteFault::None;
    require(keyWrites==16 && failedKeyWrites==1 && failedPreferenceKey==L"NightDurationMs" && fixture.bytes()==before,"Failed Night duration write published a partial settings snapshot.");
    reload();require(selectedNightDuration()==1501,"Failed custom Night preference save replaced its previous value.");
    choose(app.nightDuration,0);preferences(true);reload();configure();
    require(selectedNightDuration()==0 && app.settings.night.durationMs==0 && !app.hasCustomNightDuration && !GetPrivateProfileIntW(L"Settings",L"NightDurationMs",-1,app.preferences.c_str()),
        "Choosing Auto retained a stale custom Night value in preferences.");
    fixture.onlySettingsRemain();std::cout<<"PASS Night exact millisecond/preset/Auto roundtrips, strict malformed fallback, Desktop startup and atomic named-key rollback\n";
}
struct OwnedFile {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit OwnedFile(HANDLE handle):value(handle){require(value!=INVALID_HANDLE_VALUE,"Cannot open owned locked INI.");}
    ~OwnedFile(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    void close(){if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
};
void replacementFailure(){
    PreferencesFixture fixture;fixture.seed(legacy);
    OwnedFile locked(CreateFileW(app.preferences.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    app.settings.folder=unicodeFolder;choose(app.interval,0);choose(app.videoSize,0);choose(app.encodingQuality,encodingQualityChoice(static_cast<EncodingQuality>(2)));
    preferences(true);locked.close();
    std::cout<<"locked replacement attempts="<<replacementAttempts<<" Windows error="<<replacementError<<'\n';
    require(replacementAttempts==1 && (replacementError==ERROR_SHARING_VIOLATION || replacementError==ERROR_ACCESS_DENIED),
            "Did not reach a real replacement sharing/access failure.");
    require(fixture.bytes()==legacy,"Failed replacement changed original bytes.");fixture.onlySettingsRemain();reload();expectOptions(L"C:\\Prior",4,1,0);
    app.settings.folder=unicodeFolder;choose(app.interval,0);choose(app.videoSize,0);choose(app.encodingQuality,encodingQualityChoice(static_cast<EncodingQuality>(2)));
    preferences(true);reload();expectOptions(unicodeFolder,0,0,2);fixture.onlySettingsRemain();
    require(replacementAttempts==2 && replacementError==ERROR_SUCCESS,"Unlocked retry did not recover normal publication.");
    std::cout<<"PASS real replacement lock preserves original bytes/settings, removes staged file, and permits retry\n";
}
void readFailure(){
    PreferencesFixture fixture;fixture.seed(legacy);
    OwnedFile locked(CreateFileW(app.preferences.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    OVERLAPPED position{};
    require(LockFileEx(locked.value,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,MAXDWORD,MAXDWORD,&position)!=FALSE,
            "Cannot acquire the owned snapshot-read byte-range lock.");
    beforePreferenceReplace=[&]{UnlockFileEx(locked.value,0,MAXDWORD,MAXDWORD,&position);locked.close();};
    // Releasing before an erroneous publication makes this test distinguish a
    // snapshot read failure from merely being unable to replace a locked file.
    app.settings.folder=unicodeFolder;choose(app.interval,0);choose(app.videoSize,0);choose(app.encodingQuality,encodingQualityChoice(static_cast<EncodingQuality>(2)));
    preferences(true);beforePreferenceReplace=nullptr;
    if(locked.value!=INVALID_HANDLE_VALUE){UnlockFileEx(locked.value,0,MAXDWORD,MAXDWORD,&position);locked.close();}
    require(replacementAttempts==0,"Failed snapshot read reached publication.");
    require(fixture.bytes()==legacy,"Failed snapshot read changed original bytes.");fixture.onlySettingsRemain();reload();expectOptions(L"C:\\Prior",4,1,0);
    std::cout<<"PASS real snapshot read lock preserves original bytes/settings without attempting publication\n";
}
void preparationAllocationFailure(){
    PreferencesFixture fixture;fixture.seed(legacy);app.settings.folder=unicodeFolder;
    choose(app.interval,0);choose(app.videoSize,0);choose(app.encodingQuality,encodingQualityChoice(static_cast<EncodingQuality>(2)));
    const auto failures=preferenceAllocation::failures;preferenceAllocation::failNext=true;
    preferences(true);
    preferenceAllocation::failNext=false;
    require(preferenceAllocation::failures==failures+1,"Preparation allocation fault was not consumed exactly once.");
    require(keyWrites==0&&replacementAttempts==0&&saveDiagnostics==1,"Preparation failure reached a write or lacked its diagnostic.");
    require(fixture.bytes()==legacy,"Preparation allocation failure changed original bytes.");
    fixture.onlySettingsRemain();reload();expectOptions(L"C:\\Prior",4,1,0);
    app.settings.folder=unicodeFolder;preferences(true);reload();expectOptions(unicodeFolder,4,1,0);fixture.onlySettingsRemain();
    require(replacementAttempts==1&&saveDiagnostics==1,"Healthy retry after allocation failure did not publish once.");
    std::cout<<"PASS real preparation allocation failure stays inside preferences, preserves bytes, and permits retry\n";
}
void partialKeyWriteFailure(){
    PreferencesFixture fixture;fixture.seed(legacy);app.settings.folder=unicodeFolder;
    choose(app.interval,0);choose(app.videoSize,0);choose(app.encodingQuality,encodingQualityChoice(static_cast<EncodingQuality>(2)));
    writeFault=WriteFault::DenySecond;preferences(true);writeFault=WriteFault::None;
    require(keyWrites==2&&failedKeyWrites==1&&(keyWriteError==ERROR_SHARING_VIOLATION||keyWriteError==ERROR_ACCESS_DENIED),
            "Expected exactly one real sharing-denied second key write.");
    require(replacementAttempts==0&&saveDiagnostics==1,"Failed key update reached publication or lacked its diagnostic.");
    require(fixture.bytes()==legacy,"A partial key write published a mixed settings snapshot.");
    fixture.onlySettingsRemain();reload();expectOptions(L"C:\\Prior",4,1,0);
    std::cout<<"PASS real second-key sharing failure preserves the complete original and cleans the staging file\n";
}
void stagedExceptionCleanup(){
    PreferencesFixture fixture;fixture.seed(legacy);app.settings.folder=unicodeFolder;
    writeFault=WriteFault::ThrowAfterFirst;preferences(true);writeFault=WriteFault::None;
    require(keyWrites==1&&syntheticExceptions==1&&replacementAttempts==0&&saveDiagnostics==1,
            "Synthetic staged exception was not contained before publication.");
    require(fixture.bytes()==legacy,"Synthetic staged exception changed original bytes.");
    fixture.onlySettingsRemain();reload();expectOptions(L"C:\\Prior",4,1,0);
    std::cout<<"PASS synthetic post-stage C++ exception is contained and cleans only its owned temporary file\n";
}
void timeCompressionPreferences(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();
    require(app.settings.timeSkip.mode==TimeSkipMode::Off && app.settings.timeSkip.multiplier==4 && app.settings.timeSkip.uncertainAsAbsent,"Legacy preferences changed compression defaults.");
    TimeSkipSettings expected;expected.mode=TimeSkipMode::QuietWithinSchedule;expected.multiplier=64;expected.rampFrames=60;
    expected.quietSensitivity=QuietSensitivity::High;expected.uncertainAsAbsent=false;expected.quietAfterMs=int64_t(INT_MAX)*1000;expected.repeatSeconds=INT_MAX;expected.rangeCount=2;
    expected.ranges[0]={INT_MAX-1,INT_MAX};expected.ranges[1]={0,1};
    app.settings.timeSkip=expected;preferences(true);reload();
    require(app.settings.timeSkip.mode==expected.mode && app.settings.timeSkip.quietSensitivity==expected.quietSensitivity && !app.settings.timeSkip.uncertainAsAbsent && app.settings.timeSkip.quietAfterMs==expected.quietAfterMs && app.settings.timeSkip.repeatSeconds==INT_MAX &&
        app.settings.timeSkip.rangeCount==2 && app.settings.timeSkip.ranges[0].startSeconds==0 && app.settings.timeSkip.ranges[1].endSeconds==INT_MAX,"Exact complete policy lost values on real INI roundtrip.");
    require(choice(app.mode)==0,"Loading compression changed Desktop startup.");expectUnknownContent(fixture);
    const auto values=skipValues(app.settings.timeSkip);
    const auto restore=[&](){for(size_t i=0;i<values.size();++i)require(WritePrivateProfileStringW(L"Settings",SkipKeys[i],values[i].c_str(),app.preferences.c_str())!=FALSE,"Cannot restore owned compression values.");};
    for(size_t i=0;i<values.size();++i){
        for(const wchar_t* invalid:{L"-1",L"garbage",L"?",L"999999999999999999999999999999999999999"}){
            restore();require(WritePrivateProfileStringW(L"Settings",SkipKeys[i],invalid,app.preferences.c_str())!=FALSE,"Cannot seed invalid policy.");reload();
            require(app.settings.timeSkip.mode==TimeSkipMode::Off && app.settings.timeSkip.multiplier==4 && app.settings.timeSkip.rangeCount==0,"Malformed saved policy partially enabled or retained stale fields.");
        }
        restore();require(WritePrivateProfileStringW(L"Settings",SkipKeys[i],nullptr,app.preferences.c_str())!=FALSE,"Cannot delete owned policy key.");reload();
        if(i==6)require(app.settings.timeSkip.mode==expected.mode && app.settings.timeSkip.quietSensitivity==QuietSensitivity::Standard,"Missing optional sensitivity changed old policy instead of using Standard.");
        else if(i==7)require(app.settings.timeSkip.mode==expected.mode && app.settings.timeSkip.uncertainAsAbsent,"Missing optional uncertainty setting changed old policy instead of using the enabled default.");
        else require(app.settings.timeSkip.mode==TimeSkipMode::Off,"Missing enabled-policy key silently defaulted and enabled.");
    }
    for(auto invalid:{L"",L"3",L"01"}){restore();require(WritePrivateProfileStringW(L"Settings",SkipKeys[6],invalid,app.preferences.c_str())!=FALSE,"Cannot seed malformed optional sensitivity.");reload();require(app.settings.timeSkip.mode==TimeSkipMode::Off && app.settings.timeSkip.quietSensitivity==QuietSensitivity::Standard,"Malformed optional sensitivity did not reject the complete policy.");}
    for(auto invalid:{L"",L"2",L"01"}){restore();require(WritePrivateProfileStringW(L"Settings",SkipKeys[7],invalid,app.preferences.c_str())!=FALSE,"Cannot seed malformed optional uncertainty choice.");reload();require(app.settings.timeSkip.mode==TimeSkipMode::Off && app.settings.timeSkip.uncertainAsAbsent,"Malformed optional uncertainty choice did not reject the complete policy.");}
    restore();const std::wstring oversized=L"0:1"+std::wstring(520,L' ')+L"junk";
    require(WritePrivateProfileStringW(L"Settings",SkipKeys[5],oversized.c_str(),app.preferences.c_str())!=FALSE,"Cannot seed oversized range string.");reload();require(app.settings.timeSkip.mode==TimeSkipMode::Off,"Truncated saved schedule accepted.");
    restore();require(WritePrivateProfileStringW(L"Settings",SkipKeys[5],L"5:20;0:5;40:60",app.preferences.c_str())!=FALSE,"Cannot seed touching saved schedule.");reload();
    require(app.settings.timeSkip.rangeCount==2 && app.settings.timeSkip.ranges[0].startSeconds==0 && app.settings.timeSkip.ranges[0].endSeconds==20,"Loaded overlaps/touching ranges did not merge.");
    for(auto mode:{TimeSkipMode::Off,TimeSkipMode::Quiet,TimeSkipMode::Manual,TimeSkipMode::QuietWithinSchedule,TimeSkipMode::NoPerson,TimeSkipMode::PersonOnly,TimeSkipMode::NoPersonWithinSchedule}){
        app.settings.timeSkip.mode=mode;preferences(true);reload();require(app.settings.timeSkip.mode==mode,"Saved compression mode changed.");}
    const unsigned inspections=lapse::uiPersonPackInspections;
    configure();require(choice(app.mode)==0 && app.settings.timeSkip.mode==TimeSkipMode::NoPersonWithinSchedule && lapse::uiPersonPackInspections==inspections,
        "Loading a person policy changed Desktop startup or inspected its optional pack.");
    for(bool enabled:{true,false}){
        app.settings.timeSkip.uncertainAsAbsent=enabled;preferences(true);app.settings.timeSkip.uncertainAsAbsent=!enabled;reload();
        wchar_t stored[8]{};GetPrivateProfileStringW(L"Settings",SkipKeys[7],L"missing",stored,8,app.preferences.c_str());
        require(app.settings.timeSkip.mode==TimeSkipMode::NoPersonWithinSchedule && app.settings.timeSkip.uncertainAsAbsent==enabled && std::wstring(stored)==(enabled?L"1":L"0"),"Person uncertainty choice failed exact atomic INI roundtrip.");
    }
    require(WritePrivateProfileStringW(L"Settings",SkipKeys[7],nullptr,app.preferences.c_str()),"Cannot seed an old person policy without the uncertainty key.");reload();
    require(app.settings.timeSkip.mode==TimeSkipMode::NoPersonWithinSchedule && app.settings.timeSkip.quietAfterMs==expected.quietAfterMs && app.settings.timeSkip.uncertainAsAbsent,"Old person policy did not retain its mode and dwell with the enabled uncertainty default.");
    require(WritePrivateProfileStringW(L"Settings",SkipKeys[0],L"7",app.preferences.c_str())!=FALSE,"Cannot seed future mode.");reload();
    require(app.settings.timeSkip.mode==TimeSkipMode::Off,"Unknown numeric policy mode enabled a partial policy.");
    restore();reload();app.settings.timeSkip.mode=TimeSkipMode::NoPersonWithinSchedule;preferences(true);reload();
    const auto before=fixture.bytes();keyWrites=0;app.settings.timeSkip={};writeFault=WriteFault::DenyCompression;preferences(true);writeFault=WriteFault::None;
    require(keyWrites==25 && failedKeyWrites==1 && failedPreferenceKey==L"TimeSkipUncertainAsAbsent" && fixture.bytes()==before,"Final compression-key failure published a partial policy.");
    fixture.onlySettingsRemain();reload();require(app.settings.timeSkip.mode==TimeSkipMode::NoPersonWithinSchedule && !app.settings.timeSkip.uncertainAsAbsent,"Failed complete policy write lost original mode or uncertainty choice.");
    app.settings.timeSkip={};
    std::cout<<"PASS real compression preferences: compatible optional sensitivity/uncertainty defaults, exact opt-out roundtrip, malformed policy Off, bounded/merged ranges, preserved content and final-key atomic failure\n";
}
void preferencePathBoundary(size_t length){
    std::filesystem::path ownedRoot;
    {
        PreferencesFixture fixture(length);ownedRoot=fixture.directory;
        const auto friendly=app.preferences;
        const auto temporary=friendly+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L".tmp";
        require(temporary.size()>=MAX_PATH,"Settings temporary suffix did not cross the path boundary.");
        fixture.seed(utf16(std::wstring(legacy.begin(),legacy.end())));
        reload();expectOptions(L"C:\\Prior",4,1,0);
        app.settings.folder=unicodeFolder;choose(app.interval,5);choose(app.videoSize,0);choose(app.encodingQuality,encodingQualityChoice(static_cast<EncodingQuality>(2)));
        preferences(true);reload();expectOptions(unicodeFolder,5,0,2);
        require(app.preferences==friendly,"File I/O normalization changed the logical preference location.");
        require(replacementAttempts==1&&replacementError==ERROR_SUCCESS&&saveDiagnostics==0,"Boundary save did not publish once.");
        expectUtf16(fixture);expectUnknownContent(fixture);fixture.onlySettingsRemain();

        // The replacement lock must keep original bytes and remove a long
        // temporary sibling, then permit an unlocked update at the same path.
        const auto before=fixture.bytes();
        OwnedFile locked(CreateFileW(fileIOPath(friendly).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        app.settings.folder=L"C:\\Another selected folder";choose(app.interval,1);choose(app.videoSize,1);choose(app.encodingQuality,encodingQualityChoice(static_cast<EncodingQuality>(0)));
        preferences(true);locked.close();
        require(replacementAttempts==2&&(replacementError==ERROR_SHARING_VIOLATION||replacementError==ERROR_ACCESS_DENIED)&&saveDiagnostics==1,
                "Long target did not reach the real replacement lock.");
        require(fixture.bytes()==before,"Failed long-path replacement changed original bytes.");
        reload();expectOptions(unicodeFolder,5,0,2);fixture.onlySettingsRemain();
        app.settings.folder=L"C:\\Another selected folder";choose(app.interval,1);choose(app.videoSize,1);choose(app.encodingQuality,encodingQualityChoice(static_cast<EncodingQuality>(0)));
        preferences(true);reload();expectOptions(L"C:\\Another selected folder",1,1,0);
        require(replacementAttempts==3&&replacementError==ERROR_SUCCESS&&app.preferences==friendly,"Long-path unlocked retry failed.");
        expectUnknownContent(fixture);fixture.onlySettingsRemain();
    }
    require(GetFileAttributesW(fileIOPath(ownedRoot.wstring()).c_str())==INVALID_FILE_ATTRIBUTES,"Owned long-path fixture was not cleaned.");
    std::cout<<"PASS settings path "<<length<<": real load/save, preserved values/content, locked replacement cleanup and retry\n";
}
void watermarkPreferences(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();require(!app.settings.watermark.enabled,"Old settings enabled a watermark.");
    WatermarkSettings expected;expected.enabled=true;expected.showTime=true;expected.showSpeed=false;expected.timeKind=WatermarkTimeKind::RecordedLocal;
    expected.x=1234;expected.y=10000;expected.textSize=WatermarkTextSize::Large;
    app.settings.watermark=expected;preferences(true);reload();require(sameWatermarkSettings(expected,app.settings.watermark)&&choice(app.mode)==0,"Exact watermark options or Desktop startup lost real atomic roundtrip.");
    const auto values=watermarkValues(expected);
    const auto restore=[&](){for(size_t i=0;i<values.size();++i)require(WritePrivateProfileStringW(L"Settings",WatermarkKeys[i],values[i].c_str(),app.preferences.c_str())!=FALSE,"Cannot restore owned watermark group.");};
    for(size_t i=0;i<values.size();++i){
        restore();require(WritePrivateProfileStringW(L"Settings",WatermarkKeys[i],nullptr,app.preferences.c_str()),"Cannot delete owned watermark key.");reload();
        require(sameWatermarkSettings(app.settings.watermark,WatermarkSettings{}),"Partial watermark group preserved a stale or partially enabled policy.");
        for(const wchar_t* invalid:{L"",L"-1",L"01",L"1junk",L"999999999999999999999999999999999999999"}){restore();
            require(WritePrivateProfileStringW(L"Settings",WatermarkKeys[i],invalid,app.preferences.c_str()),"Cannot seed invalid watermark field.");reload();
            require(sameWatermarkSettings(app.settings.watermark,WatermarkSettings{}),"Malformed watermark key did not reject the complete group.");}
    }
    restore();require(WritePrivateProfileStringW(L"Settings",WatermarkKeys[1],L"0",app.preferences.c_str()),"Cannot seed empty enabled watermark.");reload();require(!app.settings.watermark.enabled,"Enabled watermark without fields survived load.");
    restore();reload();const auto prior=fixture.bytes();app.settings.watermark={};keyWrites=failedKeyWrites=0;writeFault=WriteFault::DenyWatermark;preferences(true);writeFault=WriteFault::None;
    require(keyWrites==32&&failedKeyWrites==1&&failedPreferenceKey==L"WatermarkTextSize"&&fixture.bytes()==prior,"Final watermark-key failure published partial settings.");
    reload();require(sameWatermarkSettings(expected,app.settings.watermark),"Failed transaction lost existing watermark.");
    app.settings.watermark.enabled=false;preferences(true);reload();expected.enabled=false;require(sameWatermarkSettings(expected,app.settings.watermark),"Disabled valid inactive choices failed persistence.");
    expectUnknownContent(fixture);fixture.onlySettingsRemain();app.settings.watermark={};app.watermarkCheckValid=false;
    std::cout<<"PASS complete watermark group atomic roundtrip, legacy/partial/malformed Off, exact coordinates, no-field rejection and final-key failure preservation\n";
}
void sizeCommandPreferences(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();
    app.camera=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|CBS_DROPDOWNLIST,0,0,120,80,app.window,nullptr,nullptr,nullptr);
    require(app.camera!=nullptr,"Cannot create owned source-size preference control.");
    add(app.camera,L"Synthetic camera");choose(app.camera,0);app.cameras={{L"Synthetic camera",L"owned-size-camera"}};
    app.engine=std::make_unique<lapse::FixtureEngine>();app.status=lapse::fixtureStatus={};
    const auto select=[&](int item){choose(app.videoSize,item);windowProc(app.window,WM_COMMAND,MAKEWPARAM(SizeBox,CBN_SELCHANGE),reinterpret_cast<LPARAM>(app.videoSize));};
    const auto open=[&]{choose(app.mode,1);app.settings.layers=preset(Mode::Camera);configure();lapse::fixtureStatus.cameraInput={640,480,17};
        windowProc(app.window,WM_COMMAND,MAKEWPARAM(SizeBox,CBN_DROPDOWN),reinterpret_cast<LPARAM>(app.videoSize));};
    select(1);require(app.settings.width==1920 && app.settings.height==1080 && app.committedSize==1,"Actual preset command did not configure 1080p.");
    preferences(true);reload();configure();require(choice(app.videoSize)==1 && app.settings.width==1920 && app.settings.height==1080,"Actual preset command did not persist through reload.");
    open();require(app.sizeSuggestions[1].item>=0,"Verified camera size action was unavailable in owned preferences fixture.");
    select(app.sizeSuggestions[1].item);require(app.committedSize==2 && app.hasCustomSize && app.settings.width==640 && app.settings.height==480,"Source command did not commit exact custom dimensions.");
    preferences(true);reload();configure();require(choice(app.videoSize)==2 && app.settings.width==640 && app.settings.height==480,"Source command did not persist exact custom dimensions.");
    open();choose(app.videoSize,app.sizeSuggestions[1].item);configure();
    require(app.settings.width==640 && app.settings.height==480,"Uncommitted transient row changed configured dimensions.");
    preferences(true);reload();configure();
    wchar_t quality[16]{};GetPrivateProfileStringW(L"Settings",L"Quality",L"",quality,16,app.preferences.c_str());
    require(std::wstring(quality)==L"2" && choice(app.videoSize)==2 && app.settings.width==640 && app.settings.height==480,"Transient source-action index leaked into saved preferences.");
    select(0);preferences(true);reload();configure();require(choice(app.videoSize)==0 && app.settings.width==1280 && app.settings.height==720,"Later actual preset command did not supersede a saved source snapshot.");
    expectUnknownContent(fixture);fixture.onlySettingsRemain();app.engine.reset();app.status=lapse::fixtureStatus={};
    DestroyWindow(app.camera);app.camera=nullptr;app.cameras.clear();app.settings.layers=preset(Mode::Desktop);
    std::cout<<"PASS actual size commands persist presets and exact source snapshots; transient action indices never serialize\n";
}
void checkpointBeforeRecording(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();const auto original=fixture.bytes();
    const auto child=[&](const wchar_t* type,DWORD style){auto value=CreateWindowExW(0,type,L"Owned recording preference control",WS_CHILD|style,0,0,120,80,app.window,nullptr,nullptr,nullptr);
        require(value!=nullptr,"Cannot create owned recording preference control.");return value;};
    app.preview=child(L"STATIC",0);app.statusText=child(L"STATIC",0);app.camera=child(L"COMBOBOX",CBS_DROPDOWNLIST);
    add(app.camera,L"Synthetic camera");choose(app.camera,0);app.cameras={{L"Synthetic camera",L"owned-camera"}};
    app.settings.layers=preset(Mode::Camera);choose(app.mode,1);app.engine=std::make_unique<lapse::FixtureEngine>();
    app.startupComplete=true;app.status=lapse::fixtureStatus={};app.trayRegistered=false;app.closeWhenDone=false;app.hiddenToTray=false;
    lapse::recordCalls=lapse::configurationCalls=0;lapse::beforeRecording={};
    SendMessageW(app.nightEnabled,BM_SETCHECK,BST_CHECKED,0);choose(app.nightTarget,2);
    app.customNightDurationMs=1501;app.hasCustomNightDuration=true;app.committedNightDuration=6;
    SendMessageW(app.recoveryMode,BM_SETCHECK,BST_CHECKED,0);
    app.settings.timeSkip.mode=TimeSkipMode::NoPerson;app.settings.timeSkip.quietAfterMs=90000;app.settings.timeSkip.multiplier=8;app.settings.timeSkip.uncertainAsAbsent=false;
    SendMessageW(app.captureCursor,BM_SETCHECK,BST_UNCHECKED,0);
    app.hasCustomInterval=true;app.customIntervalMs=2500;app.committedInterval=6;
    app.hasCustomSegment=true;app.customSegmentSeconds=777;app.committedSegment=5;
    app.hasCustomLimit=true;app.customLimitSeconds=3700;app.committedLimit=6;customItems();
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(IntervalBox,CBN_SELCHANGE),reinterpret_cast<LPARAM>(app.interval));
    windowProc(app.window,WM_COMMAND,NightBox,0);
    choose(app.startDelay,3);windowProc(app.window,WM_COMMAND,MAKEWPARAM(StartDelayBox,CBN_SELCHANGE),reinterpret_cast<LPARAM>(app.startDelay));
    require(replacementAttempts==0 && keyWrites==0 && fixture.bytes()==original,"Idle option changes added unsolicited preference writes.");
    app.cameras.clear();windowProc(app.window,WM_COMMAND,Record,0);app.cameras={{L"Synthetic camera",L"owned-camera"}};
    choose(app.nightDuration,5);windowProc(app.window,WM_COMMAND,Record,0);choose(app.nightDuration,6);
    choose(app.encodingMode,3);windowProc(app.window,WM_COMMAND,Record,0);choose(app.encodingMode,0);
    require(!lapse::recordCalls && !replacementAttempts && !keyWrites && fixture.bytes()==original,"Invalid source/Night Record wrote settings or started capture.");
    lapse::beforeRecording=[&]{require(replacementAttempts==1 && fixture.bytes()!=original,"Capture began before its preference checkpoint.");};
    windowProc(app.window,WM_COMMAND,Record,0);
    require(lapse::recordCalls==1 && lapse::recordedSettings.startDelaySeconds==30 && lapse::fixtureStatus.state==State::Waiting && lapse::recordedSettings.intervalMs==2500 && lapse::recordedSettings.recordingLimitSeconds==3700 && lapse::recordedSettings.segmentDurationSeconds==777 &&
        !lapse::recordedSettings.captureCursor && lapse::recordedSettings.recoveryMode && lapse::recordedSettings.night.enabled && lapse::recordedSettings.night.durationMs==1501 && lapse::recordedSettings.night.targetBrightness==128 &&
        lapse::recordedSettings.timeSkip.mode==TimeSkipMode::NoPerson && lapse::recordedSettings.timeSkip.quietAfterMs==90000 && !lapse::recordedSettings.timeSkip.uncertainAsAbsent,
        "Accepted Record did not use the exact checkpointed custom/Night/person settings.");
    const auto saved=fixture.bytes();const auto configured=lapse::configurationCalls;const int writes=keyWrites;
    for(auto state:{State::Waiting,State::Starting,State::Recording,State::Paused,State::Finishing}){
        app.status.state=lapse::fixtureStatus.state=state;windowProc(app.window,WM_COMMAND,Record,0);
    }
    require(lapse::recordCalls==1 && lapse::configurationCalls==configured && replacementAttempts==1 && keyWrites==writes && fixture.bytes()==saved,
        "Duplicate active Record reconfigured, checkpointed or restarted a session.");
    app.status.state=lapse::fixtureStatus.state=State::Recording;
    for(int i=0;i<20;++i)windowProc(app.window,WM_TIMER,1,0);
    app.status.state=lapse::fixtureStatus.state=State::Idle;
    for(int i=0;i<20;++i)windowProc(app.window,WM_TIMER,1,0);
    require(replacementAttempts==1 && keyWrites==writes,"Status polling introduced periodic preference writes.");
    // Simulate reopening after interruption without WM_DESTROY or another save.
    reload();app.settings.layers=preset(Mode::Desktop);configure();
    require(choice(app.mode)==0 && app.settings.startDelaySeconds==30 && lapse::recordCalls==1 && app.settings.intervalMs==2500 && app.settings.recordingLimitSeconds==3700 && app.settings.segmentDurationSeconds==777 &&
        SendMessageW(app.nightEnabled,BM_GETCHECK,0,0)==BST_CHECKED && choice(app.nightDuration)==6 && selectedNightDuration()==1501 && choice(app.nightTarget)==2 &&
        !app.settings.captureCursor && app.settings.recoveryMode && !app.settings.night.enabled && app.settings.timeSkip.mode==TimeSkipMode::NoPerson && app.settings.timeSkip.quietAfterMs==90000 && !app.settings.timeSkip.uncertainAsAbsent,
        "Interrupted-session restart lost checkpointed settings or silently enabled camera capture.");
    expectUnknownContent(fixture);fixture.onlySettingsRemain();
    app.settings.layers=preset(Mode::Camera);choose(app.mode,1);app.customIntervalMs=3500;customItems();
    SendMessageW(app.recoveryMode,BM_SETCHECK,BST_UNCHECKED,0);
    choose(app.startDelay,5);
    OwnedFile locked(CreateFileW(fileIOPath(app.preferences).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    lapse::beforeRecording=[&]{require(replacementAttempts==2 && saveDiagnostics==1 && fixture.bytes()==saved,"Failed checkpoint did not preserve prior INI and emit the existing diagnostic before capture.");};
    windowProc(app.window,WM_COMMAND,Record,0);locked.close();lapse::beforeRecording={};
    require(lapse::recordCalls==2 && lapse::recordedSettings.startDelaySeconds==300 && lapse::recordedSettings.intervalMs==3500 && !lapse::recordedSettings.recoveryMode && fixture.bytes()==saved,
        "A checkpoint replacement failure blocked recording or changed the previous settings.");
    fixture.onlySettingsRemain();app.startupComplete=false;app.engine.reset();app.status=lapse::fixtureStatus={};
    std::cout<<"PASS actual Record checkpoints accepted custom/Night/person options before capture; invalid/active/polled paths do not write; failed atomic replacement preserves prior INI and recording continues\n";
}
}

void statusPreferences(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();
    require(sameStatusFeedSettings(app.settings.statusFeed,StatusFeedSettings{}) && !app.settings.saveStatusLog && !app.statusHotkey && app.statusDraftKind==0 &&
        app.statusDraftTimerMs==25*60000 && app.statusDraftBreakMs==5*60000 && !app.statusDraftRepeat,"Old settings changed status defaults or turned on the status list.");
    const StatusFeedSettings look{StatusCorner::BottomRight,StatusTextSize::Large,StatusStyle::EdgeFade};
    const auto key=static_cast<uint16_t>(MAKEWORD('U',HOTKEYF_CONTROL|HOTKEYF_ALT));
    const auto put=[&](const wchar_t* name,const wchar_t* value){require(WritePrivateProfileStringW(L"Settings",name,value,app.preferences.c_str())!=FALSE,"Cannot seed status key.");};
    // Recent statuses from older versions are no longer kept; saving removes them.
    put(L"StatusRecent1",L"Work on Essay");put(L"StatusRecent8",L"Shower");
    app.settings.statusFeed=look;app.settings.saveStatusLog=true;app.statusDraftKind=2;app.statusDraftTimerMs=90000;app.statusDraftBreakMs=450000;app.statusDraftRepeat=true;app.statusHotkey=key;
    preferences(true);
    for(const wchar_t* retired:{L"StatusRecent1",L"StatusRecent8"}){wchar_t value[32]{};
        GetPrivateProfileStringW(L"Settings",retired,L"missing",value,32,app.preferences.c_str());require(std::wcscmp(value,L"missing")==0,"Saving kept a retired recent status.");}
    app.settings.statusFeed={};app.settings.saveStatusLog=false;app.statusDraftKind=0;app.statusDraftTimerMs=app.statusDraftBreakMs=0;app.statusDraftRepeat=false;app.statusHotkey=0;
    reload();
    require(sameStatusFeedSettings(look,app.settings.statusFeed) && app.settings.saveStatusLog && app.statusDraftKind==2 && app.statusDraftTimerMs==90000 &&
        app.statusDraftBreakMs==450000 && app.statusDraftRepeat && app.statusHotkey==key,"Status look, status list choice, last choices or shortcut lost the real roundtrip.");
    // Each malformed key falls back alone.
    put(L"StatusCorner",L"9");put(L"StatusTextSize",L"1junk");put(L"StatusTimerSeconds",L"0");put(L"StatusLastKind",L"-1");put(L"StatusSaveLog",L"2");
    put(L"StatusHotkey",std::to_wstring(MAKEWORD('U',HOTKEYF_ALT)).c_str());
    reload();
    require(app.settings.statusFeed.corner==StatusCorner::TopLeft && app.settings.statusFeed.textSize==StatusTextSize::Medium && app.settings.statusFeed.style==StatusStyle::EdgeFade &&
        app.statusDraftTimerMs==25*60000 && app.statusDraftKind==0 && app.statusDraftBreakMs==450000 && !app.statusHotkey && !app.settings.saveStatusLog,"Malformed status keys were coerced or reset their neighbours.");
    unregisterRecordingHotkeys();app.statusHotkey=0;app.settings.statusFeed={};app.settings.saveStatusLog=false;app.statusDraftKind=0;
    app.statusDraftTimerMs=25*60000;app.statusDraftBreakMs=5*60000;app.statusDraftRepeat=false;
    expectUnknownContent(fixture);fixture.onlySettingsRemain();
    std::cout<<"PASS status look, opt-in status list, last timer choices and shortcut roundtrip; retired recents removed; malformed keys fall back individually\n";
}
void resetToDefaults(){
    PreferencesFixture fixture;fixture.seed(legacy);reload();
    std::wstring defaultFolder,ignored;require(defaultPaths(defaultFolder,ignored),"Windows profile folders are unavailable.");
    const auto customize=[&]{
        app.settings.folder=L"C:\\Custom reset folder";choose(app.interval,5);choose(app.videoSize,1);choose(app.encodingQuality,0);choose(app.encodingMode,5);
        choose(app.stopAfter,3);choose(app.startDelay,3);choose(app.splitEvery,2);choose(app.nightTarget,2);
        SendMessageW(app.lowDisk,BM_SETCHECK,BST_UNCHECKED,0);SendMessageW(app.captureCursor,BM_SETCHECK,BST_UNCHECKED,0);SendMessageW(app.nightEnabled,BM_SETCHECK,BST_CHECKED,0);
        app.settings.outputFps=60;app.settings.watermark.enabled=true;app.settings.timeSkip.mode=TimeSkipMode::Quiet;app.settings.encodingOptions.av1Preset=3;
        app.settings.statusFeed={StatusCorner::BottomRight,StatusTextSize::Large,StatusStyle::EdgeFade};app.settings.saveStatusLog=true;app.statusDraftKind=2;
        configure();preferences(true);
    };
    customize();const auto customized=fixture.bytes();
    // Declining changes nothing in memory or on disk.
    resetAnswer=IDNO;resetQuestions=0;app.startupComplete=true;resetSettings();
    require(resetQuestions==1 && choice(app.interval)==5 && app.settings.folder==L"C:\\Custom reset folder" && app.settings.saveStatusLog && fixture.bytes()==customized,"Declined reset changed settings.");
    // While recording, reset is unavailable and does not ask.
    app.status.state=State::Recording;resetSettings();app.status={};
    require(resetQuestions==1 && choice(app.interval)==5,"Reset ran or asked during a recording.");
    resetAnswer=IDYES;resetSettings();
    const EncodingOptions defaultOptions;
    const auto expectDefaults=[&](const char* message){
        require(choice(app.interval)==2 && choice(app.videoSize)==0 && choice(app.encodingQuality)==encodingQualityChoice(EncodingQuality::Balanced) && choice(app.encodingMode)==0 &&
            choice(app.stopAfter)==0 && choice(app.startDelay)==0 && choice(app.splitEvery)==0 && choice(app.nightTarget)==1 && choice(app.mode)==0 &&
            isChecked(app.lowDisk) && isChecked(app.captureCursor) && !isChecked(app.nightEnabled) && !isChecked(app.recoveryMode) &&
            app.settings.outputFps==DefaultOutputFps && !app.settings.watermark.enabled && app.settings.timeSkip.mode==TimeSkipMode::Off &&
            app.settings.encodingOptions.av1Preset==defaultOptions.av1Preset && app.settings.encodingOptions.rateControl==EncodingRateControl::Automatic &&
            sameStatusFeedSettings(app.settings.statusFeed,StatusFeedSettings{}) && !app.settings.saveStatusLog && app.statusDraftKind==0 && app.settings.folder==defaultFolder,message);
    };
    expectDefaults("Reset left a setting changed.");
    require(resetQuestions==2 && fixture.bytes()!=customized,"Confirmed reset did not ask once or did not save.");
    choose(app.interval,5);app.settings.saveStatusLog=true;app.settings.outputFps=60;app.settings.folder=L"C:\\Another";reload();expectDefaults("Reset defaults were not saved.");
    app.startupComplete=false;expectUnknownContent(fixture);fixture.onlySettingsRemain();
    std::cout<<"PASS reset all settings asks first, is unavailable while recording, restores every default including the folder and saves them\n";
}
int main(){
    try{
        std::cout<<std::unitbuf;std::cout<<"ACP="<<GetACP()<<'\n';
        HiddenControls controls;
        newUnicodeFile();existingUnicodeRewrite();playbackStagingRollback();migrateAnsi();encodingModes();recordingLimits();startDelayOptions();segmentOptions();diskSafety();companionOptions();recoveryOptions();cursorOptions();nightOptions();customOptions();readFailure();replacementFailure();
        preparationAllocationFailure();partialKeyWriteFailure();stagedExceptionCleanup();
        preferencePathBoundary(248);preferencePathBoundary(278);timeCompressionPreferences();watermarkPreferences();statusPreferences();sizeCommandPreferences();checkpointBeforeRecording();resetToDefaults();
        require(!IsWindowVisible(app.window),"Fixture became visible.");
        std::cout<<"All 27 preference cases passed; only owned hidden controls/settings were used.\n";return 0;
    }catch(const std::exception& error){std::cerr<<"PREFERENCES TEST FAILURE: "<<error.what()<<'\n';return 1;}
}
