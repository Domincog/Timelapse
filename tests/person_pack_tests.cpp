// Offline production pack/manager fixture: every network API is replaced.
// Real file hashing, disposition, rename and hidden window ownership are used.
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <process.h>
#include <filesystem>
#include <array>
#include <atomic>
#include <functional>
#include <fstream>
#include <iostream>
#include <vector>
#include <memory>
#include <cstring>
#include <stdexcept>
#include <cstdlib>
#include <new>
#include "../src/person_pack.h"
#include "../src/person_pack_metadata.h"
std::atomic<bool> failAllAllocations{false};
void* operator new(size_t size){if(failAllAllocations.load())throw std::bad_alloc();if(void* memory=std::malloc(size?size:1))return memory;throw std::bad_alloc();}
void* operator new[](size_t size){return ::operator new(size);}
void operator delete(void* memory) noexcept{std::free(memory);}
void operator delete[](void* memory) noexcept{std::free(memory);}
void operator delete(void* memory,size_t) noexcept{std::free(memory);}
void operator delete[](void* memory,size_t) noexcept{std::free(memory);}
namespace lapse {
constexpr uint64_t fixtureBytes=4097;
constexpr char fixtureSha[]="7aecdfc4d236df5b5e429b2d8a3f5cfd93cc25cca2bc16de8e5392bff015f3ca";
}
namespace {
std::filesystem::path ownedRoot;
std::vector<uint8_t> payload;
size_t readAt=0;
unsigned sessions=0,requests=0,networkClosed=0,verifyCalls=0,dispositions=0,renames=0;
bool failDisposition=false,failInitialDisposition=false,failRename=false,failTimer=false;
DWORD httpStatus=200;
std::function<void()> afterVerify,onDisposition;
INT_PTR dialogResult=0;
HANDLE readEntered=nullptr,readRelease=nullptr;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
HRESULT WINAPI ownFolder(REFKNOWNFOLDERID,DWORD,HANDLE,PWSTR* folder){
    auto text=ownedRoot.wstring();*folder=static_cast<PWSTR>(CoTaskMemAlloc((text.size()+1)*sizeof(wchar_t)));
    if(!*folder)return E_OUTOFMEMORY;std::memcpy(*folder,text.c_str(),(text.size()+1)*sizeof(wchar_t));return S_OK;
}
HINTERNET WINAPI netOpen(LPCWSTR,DWORD,LPCWSTR,LPCWSTR,DWORD){++sessions;return reinterpret_cast<HINTERNET>(1);}
BOOL WINAPI netTimeouts(HINTERNET,int a,int b,int c,int d){return a==5000 && b==5000 && c==5000 && d==5000;}
BOOL WINAPI netOption(HINTERNET,DWORD option,LPVOID value,DWORD length){
    require(length==sizeof(DWORD),"Unexpected network option size");const auto flag=*static_cast<DWORD*>(value);
    if(option==WINHTTP_OPTION_SECURE_PROTOCOLS)require(flag==WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2,"TLS floor missing");
    else if(option==WINHTTP_OPTION_REDIRECT_POLICY)require(flag==WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP,"Insecure redirects allowed");
    else if(option==WINHTTP_OPTION_DISABLE_FEATURE)require(flag==(WINHTTP_DISABLE_AUTHENTICATION|WINHTTP_DISABLE_COOKIES),"Implicit auth/cookies enabled");
    else require(false,"Unexpected network option");return TRUE;
}
HINTERNET WINAPI netConnect(HINTERNET,LPCWSTR host,INTERNET_PORT port,DWORD){require(std::wstring(host)==L"github.com" && port==443,"Unpinned origin");return reinterpret_cast<HINTERNET>(2);}
HINTERNET WINAPI netRequest(HINTERNET,LPCWSTR verb,LPCWSTR path,LPCWSTR,LPCWSTR,LPCWSTR*,DWORD flags){
    require(std::wstring(verb)==L"GET" && std::wstring(path).find(L"/Domincog/Timelapse/releases/download/")==0 && flags==WINHTTP_FLAG_SECURE,"Unexpected request");
    ++requests;return reinterpret_cast<HINTERNET>(3);
}
BOOL WINAPI netSend(HINTERNET,LPCWSTR,DWORD,LPVOID,DWORD,DWORD,DWORD_PTR){return TRUE;}
BOOL WINAPI netResponse(HINTERNET,LPVOID){return TRUE;}
BOOL WINAPI netHeaders(HINTERNET,DWORD level,LPCWSTR,LPVOID data,LPDWORD length,LPDWORD){
    require(level==(WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER) && *length==sizeof(DWORD),"Unexpected status query");*static_cast<DWORD*>(data)=httpStatus;return TRUE;
}
BOOL WINAPI netRead(HINTERNET,LPVOID data,DWORD capacity,LPDWORD count){
    if(readEntered && !readAt){SetEvent(readEntered);if(WaitForSingleObject(readRelease,5000)!=WAIT_OBJECT_0)return FALSE;}
    *count=static_cast<DWORD>(std::min<size_t>({capacity,997,payload.size()-readAt}));
    if(*count)std::memcpy(data,payload.data()+readAt,*count);readAt+=*count;return TRUE;
}
BOOL WINAPI netClose(HINTERNET){++networkClosed;return TRUE;}
BOOL WINAPI setInfo(HANDLE file,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID info,DWORD size){
    if(kind==FileDispositionInfo){++dispositions;if(static_cast<FILE_DISPOSITION_INFO*>(info)->DeleteFile){if(failInitialDisposition){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}}
        else {if(onDisposition)onDisposition();if(failDisposition){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}}}
    if(kind==FileRenameInfo){++renames;if(failRename){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}}
    return SetFileInformationByHandle(file,kind,info,size);
}
UINT_PTR WINAPI timer(HWND window,UINT_PTR id,UINT milliseconds,TIMERPROC callback){if(failTimer)return 0;return SetTimer(window,id,milliseconds,callback);}
BOOL WINAPI ended(HWND,INT_PTR result){dialogResult=result;return TRUE;}
HINSTANCE WINAPI noShell(HWND,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,int){throw std::runtime_error("Unexpected external launch");}
}
#define PersonPackExpectedBytes fixtureBytes
#define PersonPackExpectedSha256 fixtureSha
#define SHGetKnownFolderPath ownFolder
#include "../src/person_pack.cpp"
#undef SHGetKnownFolderPath
namespace lapse {
bool checkedVerify(HANDLE file,const std::atomic<bool>* cancelled){++verifyCalls;const bool valid=verifyPersonPackFile(file,cancelled);if(afterVerify)afterVerify();return valid;}
}
#define WinHttpOpen netOpen
#define WinHttpSetTimeouts netTimeouts
#define WinHttpSetOption netOption
#define WinHttpConnect netConnect
#define WinHttpOpenRequest netRequest
#define WinHttpSendRequest netSend
#define WinHttpReceiveResponse netResponse
#define WinHttpQueryHeaders netHeaders
#define WinHttpReadData netRead
#define WinHttpCloseHandle netClose
#define SetFileInformationByHandle setInfo
#define verifyPersonPackFile checkedVerify
#define SetTimer timer
#define EndDialog ended
#define ShellExecuteW noShell
#include "../src/person_pack_dialog.cpp"
#undef verifyPersonPackFile
#undef SetFileInformationByHandle
#undef EndDialog
#undef SetTimer
#undef ShellExecuteW
namespace {
using namespace lapse;
struct Files {
    Files(){ownedRoot=std::filesystem::current_path()/(L"owned-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        require(!std::filesystem::exists(ownedRoot),"Owned folder collision");std::filesystem::create_directory(ownedRoot);}
    ~Files(){std::error_code ec;std::filesystem::remove_all(ownedRoot,ec);}
};
std::vector<uint8_t> read(const std::wstring& path){std::ifstream input(std::filesystem::path(path),std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};}
void write(const std::wstring& path,const std::vector<uint8_t>& bytes){std::filesystem::create_directories(std::filesystem::path(path).parent_path());std::ofstream output(std::filesystem::path(path),std::ios::binary);output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));require(bool(output),"Fixture write failed");}
void reset(){payload.resize(fixtureBytes);for(size_t i=0;i<payload.size();++i)payload[i]=uint8_t(i*17+31);readAt=0;httpStatus=200;
    failDisposition=failInitialDisposition=failRename=failTimer=false;afterVerify={};onDisposition={};dialogResult=0;}
void onlyInstalled(){for(const auto& file:std::filesystem::recursive_directory_iterator(ownedRoot))if(file.is_regular_file())require(file.path()==std::filesystem::path(personPackPath()),"Owned temporary leaked");}
void transactions(){
    Files files;reset();const auto valid=payload;const auto old=valid;const auto path=personPackPath();
    for(int fault=0;fault<10;++fault){reset();write(path,old);Download task;task.path=path;
        if(fault==0)payload.pop_back();
        if(fault==1)payload.push_back(0);
        if(fault==2)payload[200]^=1;
        if(fault==3)httpStatus=404;
        if(fault==4)task.cancel();
        if(fault==5)afterVerify=[&]{task.cancel();};
        if(fault==6)failDisposition=true;
        if(fault==7)failRename=true;
        if(fault==9)failInitialDisposition=true;
        HANDLE locked=INVALID_HANDLE_VALUE;if(fault==8)locked=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        const auto before=sessions;require(!receivePack(task),"Fault unexpectedly installed detector");if(locked!=INVALID_HANDLE_VALUE)CloseHandle(locked);
        require(read(path)==old,"Precommit failure altered previous installation");onlyInstalled();
        if(fault==4)require(sessions==before,"Precancelled task opened network");
    }
    reset();write(path,{1,2,3,4});Download task;task.path=path;
    onDisposition=[&]{require(task.phase.load()==DownloadPhase::Committing,"Commit boundary not established");task.cancel();require(!task.cancelled.load(),"Cancellation interrupted atomic commit");};
    require(receivePack(task),"Valid verified download could not commit");
    require(read(path)==valid,"Successful installed file disappeared/changed after delete-on-close handle destruction");onlyInstalled();
    require(inspectPersonPack().state==PersonPackState::Ready,"Real SHA verification rejected committed payload");
    HANDLE hashing=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);require(hashing!=INVALID_HANDLE_VALUE,"Owned hash file failed");
    std::atomic<bool> cancelled{true};require(!verifyPersonPackFile(hashing,&cancelled),"Hashing ignored cancellation");CloseHandle(hashing);
    std::wstring resolved,error;HANDLE held=openVerifiedPersonPack(resolved,error);require(held && !resolved.empty(),"Verified held launch image failed");
    require(!DeleteFileW(path.c_str()),"Verified lease allowed installed-image deletion");CloseHandle(held);
    require(DeleteFileW(path.c_str())!=FALSE && inspectPersonPack().state==PersonPackState::Missing,"Explicit removal failed after releasing lease");
    std::cout<<"PASS ten precommit errors preserve prior valid bytes and clean temp; cancel cannot interrupt commit; actual BCrypt/cancellation and Win32 disposition/rename persist verified file; held-image lease prevents deletion\n";
}
struct Hidden {
    HWND owner{};HWND window{};lapse::Dialog state;
    Hidden(){owner=CreateWindowExW(0,L"STATIC",L"owned manager fixture",WS_OVERLAPPED,0,0,900,700,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);require(owner!=nullptr,"Owned parent failed");
        state.owner=owner;state.installed=inspectPersonPack();struct Template{DLGTEMPLATE dialog;WORD menu,type,title;} resource{};
        resource.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|DS_MODALFRAME;resource.dialog.cx=280;resource.dialog.cy=180;
        window=CreateDialogIndirectParamW(GetModuleHandleW(nullptr),&resource.dialog,owner,manageProc,reinterpret_cast<LPARAM>(&state));require(window && !IsWindowVisible(window),"Manager escaped owned hidden window");}
    ~Hidden(){if(IsWindow(window))DestroyWindow(window);if(IsWindow(owner))DestroyWindow(owner);}
};
void lifecycle(){Files files;reset();const auto before=sessions;
    {failTimer=true;Hidden owned;require(dialogResult==IDCANCEL,"Timer failure did not close manager safely");require(!owned.state.task,"Timer failure started download");}
    reset();{Hidden owned;require(sessions==before,"Opening manager performed network I/O");
        owned.state.task=std::make_shared<Download>();enableActions(owned.window,owned.state);manageProc(owned.window,WM_COMMAND,IDCANCEL,0);
        require(owned.state.task->cancelled.load() && owned.state.closing && !IsWindowEnabled(GetDlgItem(owned.window,IDCANCEL)),"Close did not request cancellation and wait for completion");
        owned.state.task->done.store(true);manageProc(owned.window,WM_TIMER,1,0);require(dialogResult==IDCANCEL && !owned.state.task,"Cancelled completion stranded manager");}
    reset();{Hidden owned;owned.state.task=std::make_shared<Download>();const auto task=owned.state.task;DestroyWindow(owned.owner);
        require(!IsWindow(owned.window) && task->cancelled.load(),"Owner destruction left manager or download uncancelled");}
    reset();{Hidden owned;owned.state.task=std::make_shared<Download>();owned.state.task->phase.store(DownloadPhase::Committing);
        manageProc(owned.window,WM_CLOSE,0,0);require(!owned.state.task->cancelled.load() && owned.state.closing,"Close interrupted commit phase");
        owned.state.task->done.store(true);manageProc(owned.window,WM_TIMER,1,0);require(dialogResult==IDCANCEL,"Committed completion did not close manager");}
    require(sessions==before,"Lifecycle fixture unexpectedly requested network");
    std::cout<<"PASS timer failure, network-free opening, responsive cancellation/completion, owner destruction and committing-close lifecycle\n";
}
void inFlight(){Files files;reset();write(personPackPath(),payload);const auto prior=payload;
    readEntered=CreateEventW(nullptr,TRUE,FALSE,nullptr);readRelease=CreateEventW(nullptr,TRUE,FALSE,nullptr);require(readEntered && readRelease,"Owned worker events failed");
    try {
        Hidden owned;manageProc(owned.window,WM_COMMAND,Get,0);require(owned.state.task!=nullptr,"Explicit Get did not start worker");const auto task=owned.state.task;
        require(WaitForSingleObject(readEntered,3000)==WAIT_OBJECT_0,"Worker never reached held in-flight body read");
        manageProc(owned.window,WM_COMMAND,IDCANCEL,0);require(task->cancelled.load() && owned.state.closing,"In-flight close did not cancel");
        SetEvent(readRelease);const uint64_t began=GetTickCount64();while(!task->done.load(std::memory_order_acquire) && GetTickCount64()-began<3000)Sleep(1);
        require(task->done.load(std::memory_order_acquire) && !downloadBusy.load() && !task->success,"Worker completion did not release its slot and publish cancellation");
        manageProc(owned.window,WM_TIMER,1,0);require(dialogResult==IDCANCEL && read(personPackPath())==prior,"In-flight cancellation replaced prior valid installation or stranded dialog");onlyInstalled();
    } catch (...) {SetEvent(readRelease);CloseHandle(readEntered);CloseHandle(readRelease);readEntered=readRelease=nullptr;throw;}
    CloseHandle(readEntered);CloseHandle(readRelease);readEntered=readRelease=nullptr;
    std::cout<<"PASS actual owned download thread acknowledges in-flight read; cancellation preserves prior valid installation and publishes released slot before completion\n";
}
void allocationFailure(){Files files;reset();write(personPackPath(),payload);const auto prior=payload;
    auto task=std::make_shared<Download>();task->path=personPackPath();auto argument=new std::shared_ptr<Download>(task);
    afterVerify=[](){failAllAllocations.store(true);throw std::bad_alloc();};downloadBusy.store(true);
    download(argument);failAllAllocations.store(false);afterVerify={};
    require(task->done.load(std::memory_order_acquire) && !task->success && !downloadBusy.load() && task->detail.empty(),"Allocation failure in terminal error messages escaped or failed to publish completion");
    require(read(personPackPath())==prior,"Allocation failure changed previous valid detector");onlyInstalled();
    std::cout<<"PASS failing all C++ allocations at verify completion, catch-message and fallback-message still publishes terminal failure and releases busy slot\n";
}
void layout(){Files files;reset();Hidden owned;const auto network=sessions;
    for(int dpi:{96,144,192,288}){
        RECT suggested{0,0,320,260};manageProc(owned.window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&suggested));
        RECT outer{};GetWindowRect(owned.window,&outer);MONITORINFO monitor{sizeof(monitor)};require(GetMonitorInfoW(MonitorFromWindow(owned.window,MONITOR_DEFAULTTONEAREST),&monitor)!=FALSE,"Cannot inspect owned manager work area");
        require(outer.left>=monitor.rcWork.left && outer.top>=monitor.rcWork.top && outer.right<=monitor.rcWork.right && outer.bottom<=monitor.rcWork.bottom,"Manager escaped work area");
        for(int id:std::initializer_list<int>{Info,Status,Get,Remove,Licenses,IDCANCEL}){
            HWND child=GetDlgItem(owned.window,id);RECT bounds{};GetClientRect(child,&bounds);wchar_t raw[1024]{};GetWindowTextW(child,raw,1024);
            std::wstring value=raw;value.erase(std::remove(value.begin(),value.end(),L'&'),value.end());HDC dc=GetDC(child);const auto old=SelectObject(dc,owned.state.font);
            if(id==Info || id==Status){RECT measured{0,0,bounds.right,0};DrawTextW(dc,value.c_str(),-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);require(measured.bottom<=bounds.bottom,"Manager explanatory text clipped");}
            else {SIZE extent{};GetTextExtentPoint32W(dc,value.c_str(),static_cast<int>(value.size()),&extent);require(extent.cx+owned.state.scale(12)<=bounds.right,"Manager button text clipped");}
            SelectObject(dc,old);ReleaseDC(child,dc);
        }
        for(int id:std::initializer_list<int>{Location,Get,Remove,Licenses,IDCANCEL}){
            HWND child=GetDlgItem(owned.window,id);manageProc(owned.window,WM_COMMAND,MAKEWPARAM(id,id==Location?EN_SETFOCUS:BN_SETFOCUS),reinterpret_cast<LPARAM>(child));
            RECT bounds{},client{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,owned.window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(owned.window,&client);
            require(bounds.bottom>0 && bounds.top<client.bottom && bounds.right>0 && bounds.left<client.right,"Manager tab control unreachable");
        }
        require(sessions==network && !owned.state.task,"Focus navigation triggered detector download");
        manageProc(owned.window,WM_VSCROLL,SB_TOP,0);require(!owned.state.scrollY,"Manager scrollbar cannot return to top");
    }
    std::cout<<"PASS constrained 96/144/192/288 DPI manager work-area fitting, native text extents, focus scrolling and no focus-triggered actions\n";
}
}
int main(){try{transactions();lifecycle();inFlight();allocationFailure();layout();require(networkClosed==sessions*3,"Network handles leaked");std::cout<<"All offline production pack/manager groups passed.\n";return 0;}catch(const std::exception& e){failAllAllocations.store(false);std::cerr<<"PACK TEST FAILURE: "<<e.what()<<'\n';return 1;}}
