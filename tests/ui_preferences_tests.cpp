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
class FixtureEngine {
public:
    void configure(const Settings&) {}
    void refreshSources() {}
    void record() {}
    void pause() {}
    void setPaused(bool) {}
    void finish() {}
    Status status(){return {};}
};
std::vector<Monitor> enumerateMonitors(){throw std::runtime_error("Unexpected device enumeration.");}
std::vector<CameraDevice> enumerateCameras(std::wstring&){throw std::runtime_error("Unexpected device enumeration.");}
int runCameraHost(const wchar_t*){throw std::runtime_error("Unexpected application entry.");}
}
namespace {
std::function<void()> beforePreferenceReplace;
int replacementAttempts=0;
DWORD replacementError=ERROR_SUCCESS;
enum class WriteFault { None, DenySecond, DenyCompression, ThrowAfterFirst };
WriteFault writeFault=WriteFault::None;
int keyWrites=0, failedKeyWrites=0, syntheticExceptions=0, saveDiagnostics=0;
DWORD keyWriteError=ERROR_SUCCESS;
HANDLE deniedWrite=INVALID_HANDLE_VALUE;
BOOL WINAPI fixtureWriteProfile(LPCWSTR section,LPCWSTR key,LPCWSTR value,LPCWSTR path){
    const BOOL result=WritePrivateProfileStringW(section,key,value,path);
    const DWORD error=result?ERROR_SUCCESS:GetLastError();
    if(section&&key){
        ++keyWrites;
        if(!result){++failedKeyWrites;keyWriteError=error;}
        const int denyAt=writeFault==WriteFault::DenySecond?2:writeFault==WriteFault::DenyCompression?20:0;
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
// The reject-entry sentinel intentionally makes the GUI entry unreachable.
#pragma warning(push)
#pragma warning(disable: 4702)
#include "../src/main.cpp"
#pragma warning(pop)
#undef Engine
#undef MoveFileExW
#undef WritePrivateProfileStringW
#undef OutputDebugStringW

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
        app.mode=combo(6);app.interval=combo(6);app.videoSize=combo(2);app.encodingQuality=combo(3);app.encodingMode=combo(5);app.stopAfter=combo(6);
        app.lowDisk=CreateWindowExW(0,L"BUTTON",L"Stop on low disk space",WS_CHILD|BS_AUTOCHECKBOX,0,0,200,30,app.window,nullptr,nullptr,nullptr);
        require(app.lowDisk!=nullptr,"Cannot create owned low disk option.");SendMessageW(app.lowDisk,BM_SETCHECK,BST_CHECKED,0);
        app.nightEnabled=CreateWindowExW(0,L"BUTTON",L"Night",WS_CHILD|BS_AUTOCHECKBOX,0,0,200,30,app.window,nullptr,nullptr,nullptr);
        require(app.nightEnabled!=nullptr,"Cannot create owned night option.");
        app.nightDuration=combo(6);app.nightTarget=combo(3);choose(app.nightTarget,1);
    }
    ~HiddenControls(){DestroyWindow(app.window);app.window=nullptr;}
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
    require(choice(app.interval)==interval && choice(app.videoSize)==size && choice(app.encodingQuality)==encoding,
            "Persisted Interval/Quality/EncodingQuality changed.");
    require(choice(app.mode)==0,"Preferences load did not preserve Desktop startup mode.");
}
void reload(){
    app.settings.folder=L"C:\\Default";choose(app.interval,0);choose(app.videoSize,0);choose(app.encodingQuality,1);choose(app.mode,4);
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
    app.settings.folder=unicodeFolder;choose(app.interval,5);choose(app.videoSize,1);choose(app.encodingQuality,2);
    preferences(true);reload();expectOptions(unicodeFolder,5,1,2);expectUtf16(fixture);fixture.onlySettingsRemain();
    std::cout<<"PASS new Unicode folder and all numeric options roundtrip through real profile APIs\n";
}
void existingUnicodeRewrite(){
    PreferencesFixture fixture;
    const std::wstring initial=L"[Settings]\r\nFolder="+unicodeFolder+L"\r\nInterval=5\r\nQuality=1\r\nEncodingQuality=2\r\nEncodingMode=0\r\nRecordingLimit=0\r\nStopOnLowDiskSpace=1\r\nNightEnabled=0\r\nNightDurationMs=0\r\nNightTargetBrightness=96\r\nCaptureIntervalMs=5000\r\nVideoWidth=1280\r\nVideoHeight=720\r\nRecordingLimitSeconds=900\r\nTimeSkipMode=0\r\nTimeSkipMultiplier=4\r\nTimeSkipQuietAfterMs=120000\r\nTimeSkipRampFrames=30\r\nTimeSkipRepeatSeconds=0\r\nTimeSkipRanges=\r\n";
    fixture.seed(utf16(initial));reload();expectOptions(unicodeFolder,5,1,2);
    const auto initialBytes=fixture.bytes();
    const std::wstring changed=L"C:\\Synthetic videos\\\u65e5\u672c\u8a9e-\U0001f3a5";
    require(changed.size()==unicodeFolder.size(),"Rewrite must keep the same Unicode string length.");
    app.settings.folder=changed;choose(app.interval,1);choose(app.videoSize,0);choose(app.encodingQuality,0);
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
    for(int mode=0;mode<5;++mode){
        choose(app.encodingMode,mode);preferences(true);choose(app.encodingMode,(mode+1)%5);reload();
        require(choice(app.encodingMode)==mode,"Encoding mode did not survive a real preference roundtrip.");
    }
    for(const wchar_t* invalid:{L"-1",L"5",L"999"}){
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
    fixture.onlySettingsRemain();std::cout<<"PASS night enabled/duration/target values roundtrip atomically, malformed defaults and Desktop startup\n";
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
    app.settings.folder=unicodeFolder;choose(app.interval,0);choose(app.videoSize,0);choose(app.encodingQuality,2);
    preferences(true);locked.close();
    std::cout<<"locked replacement attempts="<<replacementAttempts<<" Windows error="<<replacementError<<'\n';
    require(replacementAttempts==1 && (replacementError==ERROR_SHARING_VIOLATION || replacementError==ERROR_ACCESS_DENIED),
            "Did not reach a real replacement sharing/access failure.");
    require(fixture.bytes()==legacy,"Failed replacement changed original bytes.");fixture.onlySettingsRemain();reload();expectOptions(L"C:\\Prior",4,1,0);
    app.settings.folder=unicodeFolder;choose(app.interval,0);choose(app.videoSize,0);choose(app.encodingQuality,2);
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
    app.settings.folder=unicodeFolder;choose(app.interval,0);choose(app.videoSize,0);choose(app.encodingQuality,2);
    preferences(true);beforePreferenceReplace=nullptr;
    if(locked.value!=INVALID_HANDLE_VALUE){UnlockFileEx(locked.value,0,MAXDWORD,MAXDWORD,&position);locked.close();}
    require(replacementAttempts==0,"Failed snapshot read reached publication.");
    require(fixture.bytes()==legacy,"Failed snapshot read changed original bytes.");fixture.onlySettingsRemain();reload();expectOptions(L"C:\\Prior",4,1,0);
    std::cout<<"PASS real snapshot read lock preserves original bytes/settings without attempting publication\n";
}
void preparationAllocationFailure(){
    PreferencesFixture fixture;fixture.seed(legacy);app.settings.folder=unicodeFolder;
    choose(app.interval,0);choose(app.videoSize,0);choose(app.encodingQuality,2);
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
    choose(app.interval,0);choose(app.videoSize,0);choose(app.encodingQuality,2);
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
    require(app.settings.timeSkip.mode==TimeSkipMode::Off && app.settings.timeSkip.multiplier==4,"Legacy preferences enabled compression.");
    TimeSkipSettings expected;expected.mode=TimeSkipMode::QuietWithinSchedule;expected.multiplier=64;expected.rampFrames=60;
    expected.quietAfterMs=int64_t(INT_MAX)*1000;expected.repeatSeconds=INT_MAX;expected.rangeCount=2;
    expected.ranges[0]={INT_MAX-1,INT_MAX};expected.ranges[1]={0,1};
    app.settings.timeSkip=expected;preferences(true);reload();
    require(app.settings.timeSkip.mode==expected.mode && app.settings.timeSkip.quietAfterMs==expected.quietAfterMs && app.settings.timeSkip.repeatSeconds==INT_MAX &&
        app.settings.timeSkip.rangeCount==2 && app.settings.timeSkip.ranges[0].startSeconds==0 && app.settings.timeSkip.ranges[1].endSeconds==INT_MAX,"Exact complete policy lost values on real INI roundtrip.");
    require(choice(app.mode)==0,"Loading compression changed Desktop startup.");expectUnknownContent(fixture);
    const auto values=skipValues(app.settings.timeSkip);
    const auto restore=[&](){for(size_t i=0;i<values.size();++i)require(WritePrivateProfileStringW(L"Settings",SkipKeys[i],values[i].c_str(),app.preferences.c_str())!=FALSE,"Cannot restore owned compression values.");};
    for(size_t i=0;i<values.size();++i){
        for(const wchar_t* invalid:{L"-1",L"garbage",L"999999999999999999999999999999999999999"}){
            restore();require(WritePrivateProfileStringW(L"Settings",SkipKeys[i],invalid,app.preferences.c_str())!=FALSE,"Cannot seed invalid policy.");reload();
            require(app.settings.timeSkip.mode==TimeSkipMode::Off && app.settings.timeSkip.multiplier==4 && app.settings.timeSkip.rangeCount==0,"Malformed saved policy partially enabled or retained stale fields.");
        }
        restore();require(WritePrivateProfileStringW(L"Settings",SkipKeys[i],nullptr,app.preferences.c_str())!=FALSE,"Cannot delete owned policy key.");reload();
        require(app.settings.timeSkip.mode==TimeSkipMode::Off,"Missing enabled-policy key silently defaulted and enabled.");
    }
    restore();const std::wstring oversized=L"0:1"+std::wstring(520,L' ')+L"junk";
    require(WritePrivateProfileStringW(L"Settings",SkipKeys[5],oversized.c_str(),app.preferences.c_str())!=FALSE,"Cannot seed oversized range string.");reload();require(app.settings.timeSkip.mode==TimeSkipMode::Off,"Truncated saved schedule accepted.");
    restore();require(WritePrivateProfileStringW(L"Settings",SkipKeys[5],L"5:20;0:5;40:60",app.preferences.c_str())!=FALSE,"Cannot seed touching saved schedule.");reload();
    require(app.settings.timeSkip.rangeCount==2 && app.settings.timeSkip.ranges[0].startSeconds==0 && app.settings.timeSkip.ranges[0].endSeconds==20,"Loaded overlaps/touching ranges did not merge.");
    for(auto mode:{TimeSkipMode::Off,TimeSkipMode::Quiet,TimeSkipMode::Manual,TimeSkipMode::QuietWithinSchedule}){
        app.settings.timeSkip.mode=mode;preferences(true);reload();require(app.settings.timeSkip.mode==mode,"Saved compression mode changed.");}
    const auto before=fixture.bytes();keyWrites=0;app.settings.timeSkip={};writeFault=WriteFault::DenyCompression;preferences(true);writeFault=WriteFault::None;
    require(keyWrites==20 && failedKeyWrites==1 && fixture.bytes()==before,"Final compression-key failure published a partial policy.");
    fixture.onlySettingsRemain();reload();require(app.settings.timeSkip.mode==TimeSkipMode::QuietWithinSchedule,"Failed complete policy write lost original mode.");
    app.settings.timeSkip={};
    std::cout<<"PASS real compression preferences: exact six-key policy, every missing/malformed key Off, bounded ranges, merged loads, preserved content and final-key atomic failure\n";
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
        app.settings.folder=unicodeFolder;choose(app.interval,5);choose(app.videoSize,0);choose(app.encodingQuality,2);
        preferences(true);reload();expectOptions(unicodeFolder,5,0,2);
        require(app.preferences==friendly,"File I/O normalization changed the logical preference location.");
        require(replacementAttempts==1&&replacementError==ERROR_SUCCESS&&saveDiagnostics==0,"Boundary save did not publish once.");
        expectUtf16(fixture);expectUnknownContent(fixture);fixture.onlySettingsRemain();

        // The replacement lock must keep original bytes and remove a long
        // temporary sibling, then permit an unlocked update at the same path.
        const auto before=fixture.bytes();
        OwnedFile locked(CreateFileW(fileIOPath(friendly).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        app.settings.folder=L"C:\\Another selected folder";choose(app.interval,1);choose(app.videoSize,1);choose(app.encodingQuality,0);
        preferences(true);locked.close();
        require(replacementAttempts==2&&(replacementError==ERROR_SHARING_VIOLATION||replacementError==ERROR_ACCESS_DENIED)&&saveDiagnostics==1,
                "Long target did not reach the real replacement lock.");
        require(fixture.bytes()==before,"Failed long-path replacement changed original bytes.");
        reload();expectOptions(unicodeFolder,5,0,2);fixture.onlySettingsRemain();
        app.settings.folder=L"C:\\Another selected folder";choose(app.interval,1);choose(app.videoSize,1);choose(app.encodingQuality,0);
        preferences(true);reload();expectOptions(L"C:\\Another selected folder",1,1,0);
        require(replacementAttempts==3&&replacementError==ERROR_SUCCESS&&app.preferences==friendly,"Long-path unlocked retry failed.");
        expectUnknownContent(fixture);fixture.onlySettingsRemain();
    }
    require(GetFileAttributesW(fileIOPath(ownedRoot.wstring()).c_str())==INVALID_FILE_ATTRIBUTES,"Owned long-path fixture was not cleaned.");
    std::cout<<"PASS settings path "<<length<<": real load/save, preserved values/content, locked replacement cleanup and retry\n";
}
}

int main(){
    try{
        std::cout<<std::unitbuf;std::cout<<"ACP="<<GetACP()<<'\n';
        HiddenControls controls;
        newUnicodeFile();existingUnicodeRewrite();migrateAnsi();encodingModes();recordingLimits();diskSafety();nightOptions();customOptions();readFailure();replacementFailure();
        preparationAllocationFailure();partialKeyWriteFailure();stagedExceptionCleanup();
        preferencePathBoundary(248);preferencePathBoundary(278);timeCompressionPreferences();
        require(!IsWindowVisible(app.window),"Fixture became visible.");
        std::cout<<"All 16 preference cases passed; only owned hidden controls/settings were used.\n";return 0;
    }catch(const std::exception& error){std::cerr<<"PREFERENCES TEST FAILURE: "<<error.what()<<'\n';return 1;}
}
