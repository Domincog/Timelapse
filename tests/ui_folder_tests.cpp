// Actual native Folder/OpenFolder commands with synthetic COM objects and an owned hidden
// window. No application startup, shell/profile lookup, input or capture.
#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <new>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>
#include <process.h>

namespace fault { thread_local bool next=false; thread_local int consumed=0; }
void* operator new(size_t count){
    if(fault::next){fault::next=false;++fault::consumed;throw std::bad_alloc();}
    if(auto memory=std::malloc(count?count:1))return memory;
    throw std::bad_alloc();
}
void* operator new[](size_t count){return ::operator new(count);}
void operator delete(void* memory)noexcept{std::free(memory);}
void operator delete[](void* memory)noexcept{std::free(memory);}
void operator delete(void* memory,size_t)noexcept{std::free(memory);}
void operator delete[](void* memory,size_t)noexcept{std::free(memory);}

namespace fixture {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
volatile bool allowForbidden=false;
void forbidden(){if(!allowForbidden)throw std::runtime_error("Unexpected app/profile/device activity.");}
int configured=0, messages=0, callbackEntries=0;
int shellCalls=0;
INT_PTR shellResult=33;
std::wstring lastMessage,shellPath;
PWSTR taskString=nullptr;
constexpr UINT Heartbeat=WM_APP+99;
struct Gate {
    std::atomic<bool> armed{false},reached{false},released{false};
    void hold() {
        if(!armed)return;reached=true;const auto until=GetTickCount64()+5000;
        while(!released){if(GetTickCount64()>=until)ExitProcess(78);Sleep(1);}
    }
    void reset(){armed=false;reached=false;released=false;}
} directoryGate,shellGate;
DWORD uiThread=0;
HWND folderButton=nullptr;
std::atomic<unsigned> threadStarts{0},threadExits{0},threadCloses{0},comStarts{0},comStops{0},finishes{0},heartbeats{0};
std::atomic<bool> workerProtocol{true};
std::atomic<DWORD> directoryThread{0},shellThread{0};
std::atomic<HANDLE> nativeThread{nullptr};
HANDLE observedThread=nullptr;
bool failThread=false,failWorkerAllocation=false,throwDirectory=false;
HRESULT comResult=S_OK;
unsigned folderTextWrites=0,folderEnableWrites=0;
std::wstring preparedPath;
struct Launch {unsigned(__stdcall* procedure)(void*);void* argument;bool failAllocation;};
unsigned __stdcall worker(void* raw) {
    const auto launch=*static_cast<Launch*>(raw);std::free(raw);
    fault::next=launch.failAllocation;const auto result=launch.procedure(launch.argument);fault::next=false;
    ++threadExits;return result;
}
uintptr_t __cdecl beginThread(void* security,unsigned stack,unsigned(__stdcall* procedure)(void*),void* argument,unsigned flags,unsigned* id) {
    require(!security&&!stack&&procedure&&argument&&!flags&&!id,"Unexpected folder thread contract.");
    if(failThread){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return 0;}
    require(!observedThread,"A second native folder worker was started before retirement.");
    auto launch=static_cast<Launch*>(std::malloc(sizeof(Launch)));require(launch!=nullptr,"Cannot create owned launch observer.");
    *launch={procedure,argument,failWorkerAllocation};
    const auto thread=_beginthreadex(security,stack,worker,launch,flags,id);
    if(!thread){std::free(launch);return 0;}
    nativeThread=reinterpret_cast<HANDLE>(thread);
    if(!DuplicateHandle(GetCurrentProcess(),reinterpret_cast<HANDLE>(thread),GetCurrentProcess(),&observedThread,SYNCHRONIZE,FALSE,0))ExitProcess(79);
    ++threadStarts;return thread;
}
BOOL WINAPI closeHandle(HANDLE value) {
    if(value==nativeThread.load()){++threadCloses;nativeThread=nullptr;}
    return CloseHandle(value);
}
HRESULT WINAPI comInitialize(void* reserved,DWORD flags) {
    if(reserved||flags!=(COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE)||GetCurrentThreadId()==uiThread)workerProtocol=false;
    ++comStarts;return comResult;
}
void WINAPI comUninitialize(){if(GetCurrentThreadId()==uiThread)workerProtocol=false;++comStops;}
std::wstring ioPath(const std::wstring& value) {
    if(GetCurrentThreadId()==uiThread)workerProtocol=false;
    directoryThread=GetCurrentThreadId();preparedPath=value;directoryGate.hold();
    if(throwDirectory)throw std::runtime_error("Synthetic directory preparation failure");
    return lapse::fileIOPath(value);
}
BOOL WINAPI shellExecuteEx(SHELLEXECUTEINFOW* request) {
    const bool valid=request&&request->cbSize==sizeof(*request)&&!request->hwnd&&
        request->fMask==(SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI)&&request->lpVerb&&std::wcscmp(request->lpVerb,L"open")==0&&
        request->lpFile&&!request->lpParameters&&!request->lpDirectory&&request->nShow==SW_SHOWNORMAL&&!request->hProcess;
    if(!valid||GetCurrentThreadId()==uiThread){workerProtocol=false;SetLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    shellThread=GetCurrentThreadId();shellPath=request->lpFile;++shellCalls;shellGate.hold();
    if(shellResult<=32){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}return TRUE;
}
BOOL WINAPI setText(HWND window,LPCWSTR value){if(window==folderButton){++folderTextWrites;if(GetCurrentThreadId()!=uiThread)workerProtocol=false;}return SetWindowTextW(window,value);}
BOOL WINAPI enable(HWND window,BOOL value){if(window==folderButton){++folderEnableWrites;if(GetCurrentThreadId()!=uiThread)workerProtocol=false;}return EnableWindow(window,value);}
LRESULT WINAPI message(HWND window,UINT message,WPARAM,LPARAM){
    require(reinterpret_cast<UINT_PTR>(window)>=1 && reinterpret_cast<UINT_PTR>(window)<=8,"Unexpected control handle.");
    if(message!=CB_GETCURSEL){forbidden();return 0;}return 0;
}
void WINAPI taskFree(void* memory){
    require(!memory || memory==taskString,"Unexpected task allocation.");
    if(memory)taskString=nullptr;
    CoTaskMemFree(memory);
}
int WINAPI messageBox(HWND,LPCWSTR text,LPCWSTR title,UINT flags){
    require(std::wcscmp(title,L"Timelapse")==0 && flags==(MB_OK|MB_ICONERROR),"Unexpected failure dialog options.");
    require(GetCurrentThreadId()==uiThread,"Folder worker attempted to display a window.");
    lastMessage=text;++messages;return IDOK;
}
HINSTANCE WINAPI shellExecute(HWND,LPCWSTR operation,LPCWSTR path,LPCWSTR parameters,LPCWSTR directory,int show){
    require(operation && std::wcscmp(operation,L"open")==0 && path && !parameters && !directory && show==SW_SHOWNORMAL,
            "Unexpected shell dispatch arguments.");
    shellPath=path;++shellCalls;return reinterpret_cast<HINSTANCE>(shellResult);
}
HRESULT WINAPI knownFolder(REFKNOWNFOLDERID,DWORD,HANDLE,PWSTR*){forbidden();return E_UNEXPECTED;}
BOOL WINAPI writeProfile(LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR){forbidden();return FALSE;}
DWORD WINAPI readProfile(LPCWSTR,LPCWSTR,LPCWSTR,LPWSTR,DWORD,LPCWSTR){forbidden();return 0;}
UINT WINAPI readInt(LPCWSTR,LPCWSTR,INT,LPCWSTR){forbidden();return 0;}
HRESULT WINAPI create(REFCLSID,LPUNKNOWN,DWORD,REFIID,LPVOID*);
HRESULT WINAPI parse(PCWSTR,IBindCtx*,REFIID,void**);
}
namespace lapse {
class FixtureEngine {
public:
    void configure(const Settings&){++fixture::configured;}
    void refreshSources(){} void record(){} void pause(){} void setPaused(bool){} void finish(){++fixture::finishes;} void cancelDelayedStart() noexcept {}
    Status status(){return {};}
};
std::vector<Monitor> enumerateMonitors(){fixture::forbidden();return {};}
std::vector<CameraDevice> enumerateCameras(std::wstring&){fixture::forbidden();return {};}
int runCameraHost(const wchar_t*){fixture::forbidden();return -1;}
}
#define Engine FixtureEngine
#define SendMessageW fixture::message
#define CoCreateInstance fixture::create
#define SHCreateItemFromParsingName fixture::parse
#define SHGetKnownFolderPath fixture::knownFolder
#define CoTaskMemFree fixture::taskFree
#define MessageBoxW fixture::messageBox
#define ShellExecuteW fixture::shellExecute
#define ShellExecuteExW fixture::shellExecuteEx
#define CoInitializeEx fixture::comInitialize
#define CoUninitialize fixture::comUninitialize
#define _beginthreadex fixture::beginThread
#define CloseHandle fixture::closeHandle
#define fileIOPath fixture::ioPath
#define SetWindowTextW fixture::setText
#define EnableWindow fixture::enable
#define WritePrivateProfileStringW fixture::writeProfile
#define GetPrivateProfileStringW fixture::readProfile
#define GetPrivateProfileIntW fixture::readInt
#include "ui_person_pack_stub.h"
#include "../src/main.cpp"
#undef Engine
#undef SendMessageW
#undef CoCreateInstance
#undef SHCreateItemFromParsingName
#undef SHGetKnownFolderPath
#undef CoTaskMemFree
#undef MessageBoxW
#undef ShellExecuteW
#undef ShellExecuteExW
#undef CoInitializeEx
#undef CoUninitialize
#undef _beginthreadex
#undef CloseHandle
#undef fileIOPath
#undef SetWindowTextW
#undef EnableWindow
#undef WritePrivateProfileStringW
#undef GetPrivateProfileStringW
#undef GetPrivateProfileIntW

namespace fixture {
void waitWorker() {
    if(observedThread){
        require(WaitForSingleObject(observedThread,5000)==WAIT_OBJECT_0,"Owned folder worker did not terminate.");
        CloseHandle(observedThread);observedThread=nullptr;
    }
    require(threadStarts==threadExits&&threadStarts==threadCloses&&workerProtocol,"Folder worker contract or handle/COM ownership failed.");
}
void completeFolder(){waitWorker();pollOpenFolder();require(!app.openFolderTask&&!shellOperationBusy,"Folder completion retained its task or slot.");}
struct Case {
    HRESULT create=S_OK,options=S_OK,current=S_OK,folder=S_OK,title=S_OK;
    HRESULT show=S_OK,result=S_OK,name=S_OK;
    bool allocationFailure=false;
    std::wstring selection=L"C:\\Synthetic & Unicode \\u65e5 Folder";
} test;
int dialogsAlive=0,itemsAlive=0,shows=0,results=0,names=0;
FILEOPENDIALOGOPTIONS requested=0;
struct Item;
struct Dialog;
Item* selected=nullptr;
Dialog* active=nullptr;
struct Item final : IShellItem {
    ULONG refs=1;
    Item(){++itemsAlive;}
    ~Item(){--itemsAlive;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
        *out=nullptr;if(id!=IID_IUnknown && id!=IID_IShellItem)return E_NOINTERFACE;
        *out=this;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{const auto left=--refs;if(!left)delete this;return left;}
    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*,REFGUID,REFIID,void**)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetParent(IShellItem**)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetDisplayName(SIGDN kind,LPWSTR* out)override{
        ++names;*out=nullptr;require(kind==SIGDN_FILESYSPATH,"Non-filesystem result requested.");
        if(FAILED(test.name))return test.name;
        require(!taskString,"Previous task string survived.");
        const size_t bytes=(test.selection.size()+1)*sizeof(wchar_t);
        taskString=static_cast<PWSTR>(CoTaskMemAlloc(bytes));if(!taskString)return E_OUTOFMEMORY;
        std::memcpy(taskString,test.selection.c_str(),bytes);*out=taskString;
        // The next real C++ allocation is the actual folder-string assignment.
        fault::next=test.allocationFailure;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAttributes(SFGAOF flags,SFGAOF* out)override{*out=flags&(SFGAO_FOLDER|SFGAO_FILESYSTEM);return S_OK;}
    HRESULT STDMETHODCALLTYPE Compare(IShellItem*,SICHINTF,int*)override{return E_NOTIMPL;}
};
struct Dialog final : IFileOpenDialog {
    ULONG refs=1;
    IShellItem* current=nullptr;
    Dialog(){++dialogsAlive;}
    ~Dialog(){if(current)current->Release();--dialogsAlive;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
        *out=nullptr;
        if(id!=IID_IUnknown && id!=IID_IModalWindow && id!=IID_IFileDialog && id!=IID_IFileOpenDialog)return E_NOINTERFACE;
        *out=this;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{const auto left=--refs;if(!left)delete this;return left;}
    HRESULT STDMETHODCALLTYPE Show(HWND owner)override{require(owner==app.window,"Incorrect dialog owner.");++shows;return test.show;}
    HRESULT STDMETHODCALLTYPE SetOptions(FILEOPENDIALOGOPTIONS value)override{requested=value;return test.options;}
    HRESULT STDMETHODCALLTYPE SetTitle(LPCWSTR)override{return test.title;}
    HRESULT STDMETHODCALLTYPE SetFolder(IShellItem* item)override{
        if(FAILED(test.folder))return test.folder;
        if(current)current->Release();current=item;if(current)current->AddRef();return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetResult(IShellItem** out)override{
        ++results;*out=nullptr;if(FAILED(test.result))return test.result;
        *out=selected=new Item;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFileTypes(UINT,const COMDLG_FILTERSPEC*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetFileTypeIndex(UINT)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetFileTypeIndex(UINT*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Advise(IFileDialogEvents*,DWORD*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Unadvise(DWORD)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetOptions(FILEOPENDIALOGOPTIONS*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetDefaultFolder(IShellItem*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetFolder(IShellItem**)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetCurrentSelection(IShellItem**)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetFileName(LPCWSTR)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetFileName(LPWSTR*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetOkButtonLabel(LPCWSTR)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetFileNameLabel(LPCWSTR)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE AddPlace(IShellItem*,FDAP)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetDefaultExtension(LPCWSTR)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Close(HRESULT)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE SetClientGuid(REFGUID)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE ClearClientData()override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetFilter(IShellItemFilter*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetResults(IShellItemArray**)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetSelectedItems(IShellItemArray**)override{return E_NOTIMPL;}
};
HRESULT WINAPI create(REFCLSID cls,LPUNKNOWN outer,DWORD context,REFIID id,LPVOID* out){
    *out=nullptr;require(cls==CLSID_FileOpenDialog && !outer && context==CLSCTX_INPROC_SERVER && id==IID_IFileOpenDialog,"Unexpected COM activation.");
    if(FAILED(test.create))return test.create;*out=active=new Dialog;return S_OK;
}
HRESULT WINAPI parse(PCWSTR path,IBindCtx* context,REFIID id,void** out){
    *out=nullptr;require(!context && id==IID_IShellItem && std::wcscmp(path,app.settings.folder.c_str())==0,"Unexpected shell parsing.");
    if(FAILED(test.current))return test.current;*out=new Item;return S_OK;
}
void reset(){
    require(!dialogsAlive && !itemsAlive && !taskString,"Previous case leaked.");
    require(!observedThread&&!app.openFolderTask&&!shellOperationBusy,"Previous folder worker survived reset.");
    directoryGate.reset();shellGate.reset();threadStarts=0;threadExits=0;threadCloses=0;comStarts=0;comStops=0;finishes=0;heartbeats=0;
    directoryThread=0;shellThread=0;workerProtocol=true;failThread=failWorkerAllocation=throwDirectory=false;comResult=S_OK;
    preparedPath.clear();folderTextWrites=folderEnableWrites=0;
    app.hiddenToTray=false;app.closeWhenDone=false;app.customDialog=nullptr;app.trayMenuOpen=false;app.failureNotice=FailureNotice::None;app.status={};
    fault::next=false;fault::consumed=0;test={};test.selection=L"C:\\Synthetic & Unicode \u65e5\u672c Folder";
    shows=results=names=configured=messages=0;requested=0;active=nullptr;selected=nullptr;
    shellCalls=0;shellResult=33;lastMessage.clear();shellPath.clear();
    std::wstring(L"C:\\Original").swap(app.settings.folder);
}
void ordinary(const char* label,bool changed,int showCount){
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(Id::Folder,BN_CLICKED),0);
    require(app.settings.folder==(changed?test.selection:L"C:\\Original"),"Unexpected folder state.");
    require(shows==showCount && configured==(changed?1:0),"Unexpected stage count.");
    require(!dialogsAlive && !itemsAlive && !taskString && !messages,"Ordinary resource/diagnostic mismatch.");
    std::cout<<"PASS "<<label<<" shows="<<shows<<" results="<<results<<" names="<<names<<" live=0\n";
}
LRESULT CALLBACK route(HWND window,UINT message,WPARAM wp,LPARAM lp){
    if(message==Heartbeat){++heartbeats;return 0;}
    if(message==WM_COMMAND){++callbackEntries;return windowProc(window,message,wp,lp);}
    return DefWindowProcW(window,message,wp,lp);
}
struct HiddenWindow {
    const wchar_t* cls=L"OwnedNativeFolderPickerTests";
    HiddenWindow(){
        uiThread=GetCurrentThreadId();
        WNDCLASSW info{};info.lpfnWndProc=route;info.hInstance=GetModuleHandleW(nullptr);info.lpszClassName=cls;
        require(RegisterClassW(&info)!=0,"Cannot register owned hidden window.");
        app.window=CreateWindowExW(0,cls,L"Owned native folder picker fixture",WS_POPUP,0,0,50,50,nullptr,nullptr,info.hInstance,nullptr);
        require(app.window && !IsWindowVisible(app.window),"Cannot create hidden window.");
        folderButton=app.openFolder=CreateWindowExW(0,L"BUTTON",L"&Open folder",WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON,0,0,100,26,app.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(OpenFolder)),info.hInstance,nullptr);
        require(folderButton!=nullptr,"Cannot create owned folder control.");
        app.interval=reinterpret_cast<HWND>(1);app.videoSize=reinterpret_cast<HWND>(2);app.encodingQuality=reinterpret_cast<HWND>(3);
        app.encodingMode=reinterpret_cast<HWND>(6);
        app.stopAfter=reinterpret_cast<HWND>(7);
        app.startDelay=reinterpret_cast<HWND>(8);
        app.monitor=reinterpret_cast<HWND>(4);app.camera=reinterpret_cast<HWND>(5);
        app.engine=std::make_unique<lapse::FixtureEngine>();
    }
    ~HiddenWindow(){
        directoryGate.released=true;shellGate.released=true;cancelOpenFolder();
        if(observedThread){WaitForSingleObject(observedThread,5000);CloseHandle(observedThread);observedThread=nullptr;}
        app.engine.reset();DestroyWindow(app.window);app.window=nullptr;folderButton=app.openFolder=nullptr;UnregisterClassW(cls,GetModuleHandleW(nullptr));
    }
};
int optionsFailure(HRESULT hr){
    reset();test.options=hr;test.selection=L"C:\\Synthetic\\ordinary-file.txt";
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(Id::Folder,BN_CLICKED),0);
    const bool okay=shows==0 && configured==0 && app.settings.folder==L"C:\\Original" && !dialogsAlive && !itemsAlive && !taskString;
    std::cout<<(okay?"PASS":"FAIL")<<" required options hr="<<std::hex<<static_cast<unsigned>(hr)<<std::dec
             <<" shows="<<shows<<" accepted_file="<<(app.settings.folder==test.selection)<<" live="<<(dialogsAlive+itemsAlive+(taskString?1:0))<<'\n';
    return okay?0:1;
}
int allocationFailure(bool dispatch){
    reset();test.selection=L"C:\\Synthetic\\"+std::wstring(2048,L'x');test.allocationFailure=true;
    bool escaped=false;const int before=callbackEntries;
    try{
        if(dispatch)SendMessageW(app.window,WM_COMMAND,MAKEWPARAM(Id::Folder,BN_CLICKED),0);
        else windowProc(app.window,WM_COMMAND,MAKEWPARAM(Id::Folder,BN_CLICKED),0);
    }catch(const std::bad_alloc&){escaped=true;}
    fault::next=false;
    const bool unchanged=app.settings.folder==L"C:\\Original";
    const bool okay=!escaped && fault::consumed==1 && unchanged && !configured && messages==1 &&
        lastMessage==L"Timelapse could not change the save folder. Close other applications, then try again." &&
        !dialogsAlive && !itemsAlive && !taskString && (!dispatch || callbackEntries==before+1);
    std::cout<<(okay?"PASS":"FAIL")<<" allocation "<<(dispatch?"SendMessage callback":"direct callback entry")<<" actual_faults="<<fault::consumed
             <<" escaped="<<escaped<<" unchanged="<<unchanged<<" dialogs="<<dialogsAlive<<" items="<<itemsAlive<<" task_strings="<<(taskString?1:0)<<" diagnostics="<<messages<<'\n';
    // Clean synthetic resources after observing a failing assertion, so failure reporting remains bounded.
    if(dialogsAlive)active->Release();if(itemsAlive)selected->Release();if(taskString)taskFree(taskString);
    require(!dialogsAlive && !itemsAlive && !taskString,"Synthetic post-observation cleanup failed.");
    return okay?0:1;
}
void waitGate(Gate& gate) {
    const auto until=GetTickCount64()+3000;
    while(!gate.reached){require(GetTickCount64()<until,"Folder worker failed to reach its controlled gate.");Sleep(1);}
}
void pumpOwned() {
    MSG message{};unsigned count=0;
    while(count<64&&PeekMessageW(&message,app.window,0,0,PM_REMOVE)){++count;TranslateMessage(&message);DispatchMessageW(&message);}
    require(count<64,"Owned folder message drain did not settle.");
}
void openCommand(){SendMessageW(app.window,WM_COMMAND,MAKEWPARAM(Id::OpenFolder,BN_CLICKED),0);}
void asyncFolderContracts() {
    const auto parent=std::filesystem::current_path();
    const auto root=parent/(L"owned-native-folder-worker-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    require(root.is_absolute()&&root.parent_path()==parent,"Folder-worker fixture escaped the test directory.");
    const auto ioRoot=fileIOPath(root.wstring());
    require(std::filesystem::create_directory(ioRoot),"Cannot create owned worker fixture.");
    struct Cleanup {std::filesystem::path root;~Cleanup(){std::error_code error;std::filesystem::remove_all(root,error);}} cleanup{ioRoot};
    for(auto state:{State::Recording,State::Waiting})for(bool atShell:{false,true}) {
        reset();app.status.state=state;app.status.message=L"Retained recording outcome";app.status.frames=17;
        app.status.savedPath=L"C:\\Retained\\earlier.mp4";
        const auto requestedPath=(root/(std::to_wstring(static_cast<int>(state))+(atShell?L"-shell \u65e5":L"-directory \u65e5"))).wstring();
        const auto later=(root/L"changed-after-click").wstring();app.settings.folder=requestedPath;
        Gate& gate=atShell?shellGate:directoryGate;gate.armed=true;
        openCommand();waitGate(gate);
        const std::weak_ptr<OpenFolderTask> task=app.openFolderTask;
        require(threadStarts==1&&shellOperationBusy&&!IsWindowEnabled(app.openFolder)&&
            (atShell?shellThread.load():directoryThread.load())!=uiThread,"Folder work blocked the UI thread or failed to bound the pending action.");
        openCommand();require(threadStarts==1,"Repeated Open folder launched a replacement worker.");
        app.settings.folder=later;
        require(PostMessageW(app.window,Heartbeat,0,0)&&PostMessageW(app.window,WM_COMMAND,MAKEWPARAM(Id::Finish,BN_CLICKED),0),"Cannot post owned responsiveness controls.");
        pumpOwned();require(heartbeats==1&&finishes==1&&threadExits==0,"UI heartbeat or actual Finish waited for folder work.");
        gate.released=true;completeFolder();
        require(task.expired()&&preparedPath==requestedPath&&shellPath==requestedPath&&shellCalls==1&&
            std::filesystem::is_directory(fileIOPath(requestedPath))&&!std::filesystem::exists(fileIOPath(later)),"Worker lost its immutable path or retained the completed task.");
        require(comStarts==1&&comStops==1&&!messages&&IsWindowEnabled(app.openFolder)&&
            app.status.message==L"Retained recording outcome"&&app.status.frames==17&&app.status.savedPath==L"C:\\Retained\\earlier.mp4",
            "Folder completion changed recording outcomes, COM lifetime, or retry availability.");
    }
    std::cout<<"PASS directory and Shell work keep native heartbeat/Finish responsive in Recording/Waiting; immutable paths and one worker\n";
    for(bool atShell:{false,true}) {
        reset();app.settings.folder=(root/(atShell?L"cancel-dispatch":L"cancel-prepare")).wstring();
        Gate& gate=atShell?shellGate:directoryGate;gate.armed=true;
        if(atShell)shellResult=SE_ERR_ACCESSDENIED;
        openCommand();waitGate(gate);const std::weak_ptr<OpenFolderTask> task=app.openFolderTask;
        cancelOpenFolder();require(!app.openFolderTask&&shellOperationBusy,"Cancellation retired a still-blocked native worker.");
        openCommand();require(threadStarts==1&&shellOperationBusy,"Cancellation allowed concurrent replacement work.");
        gate.released=true;waitWorker();pollOpenFolder();
        require(task.expired()&&!app.openFolderTask&&!shellOperationBusy&&!messages&&shellCalls==(atShell?1:0),
            "Cancelled preparation dispatched Shell or late dispatch failure reached the UI.");
        directoryGate.reset();shellGate.reset();shellResult=33;
        app.settings.folder=(root/(atShell?L"retry-dispatch":L"retry-prepare")).wstring();openCommand();completeFolder();
        require(threadStarts==2&&comStarts==2&&comStops==2&&IsWindowEnabled(app.openFolder)&&!messages,"Retired cancellation prevented a clean retry.");
    }
    std::cout<<"PASS cancellation suppresses future dispatch, holds the slot through retirement and permits retry\n";
    for(int faultKind=0;faultKind<6;++faultKind) {
        reset();app.settings.folder=(root/(L"failure-"+std::to_wstring(faultKind))).wstring();
        app.status.message=L"Earlier saved result";app.status.frames=9;app.status.savedPaths={L"C:\\Saved\\a.mp4",L"C:\\Saved\\b.mp4"};
        if(faultKind==0)fault::next=true;
        if(faultKind==1)failThread=true;
        if(faultKind==2)comResult=RPC_E_CHANGED_MODE;
        if(faultKind==3)failWorkerAllocation=true;
        if(faultKind==4)throwDirectory=true;
        if(faultKind==5)shellResult=SE_ERR_ACCESSDENIED;
        openCommand();fault::next=false;completeFolder();
        require(messages==1&&IsWindowEnabled(app.openFolder)&&!app.openFolderTask&&!shellOperationBusy&&
            app.status.message==L"Earlier saved result"&&app.status.frames==9&&app.status.savedPaths.size()==2,
            "Folder fault replaced the recording report, leaked the busy slot, or lost its single diagnostic.");
        require(shellCalls==(faultKind==5?1:0)&&comStarts==(faultKind<2?0u:1u)&&comStops==(faultKind<3?0u:1u),
            "Folder fault touched an unexpected stage or unbalanced COM cleanup.");
        if(faultKind==0)require(fault::consumed==1&&!threadStarts,"Caller allocation failure launched folder work.");
        if(faultKind==1)require(!threadStarts,"Thread-start failure created a worker.");
    }
    std::cout<<"PASS caller/worker allocation, thread-start, COM, preparation exception and Shell failures preserve reports and cleanup\n";
    reset();comResult=S_FALSE;app.settings.folder=(root/L"com-already-initialized").wstring();openCommand();completeFolder();
    require(comStarts==1&&comStops==1&&!messages,"Successful S_FALSE COM initialization was not balanced.");
    const auto texts=folderTextWrites,enables=folderEnableWrites,starts=threadStarts.load(),calls=comStarts.load();
    for(int tick=0;tick<100;++tick)pollOpenFolder();
    require(folderTextWrites==texts&&folderEnableWrites==enables&&threadStarts==starts&&comStarts==calls&&shellCalls==1,
        "Settled folder polling performed extra work.");
    std::cout<<"PASS balanced S_FALSE COM and zero extra worker/control activity on 100 settled polls\n";
}
void openFolderLongPathsAndFailures(){
    reset();
    const auto parent=std::filesystem::current_path();
    const auto root=parent/(L"owned-native-open-folder-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    require(root.is_absolute() && root.parent_path()==parent,"Open-folder fixture escaped the current test directory.");
    const std::filesystem::path extendedRoot(fileIOPath(root.wstring()));
    require(std::filesystem::create_directory(extendedRoot),"Cannot create unique owned open-folder fixture.");
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup(){std::error_code error;std::filesystem::remove_all(root,error);}
    } cleanup{extendedRoot};
    auto open=[&](const std::wstring& folder,INT_PTR result=33){
        app.settings.folder=folder;messages=shellCalls=0;lastMessage.clear();shellPath.clear();shellResult=result;
        const int before=callbackEntries;
        SendMessageW(app.window,WM_COMMAND,MAKEWPARAM(Id::OpenFolder,BN_CLICKED),0);
        require(callbackEntries==before+1 && app.settings.folder==folder,"Open folder lost callback dispatch or changed configured spelling.");
        completeFolder();
    };
    std::wstring folder=(root/L"Save folder \u65e5\u672c").wstring();
    while(folder.size()<320)folder+=L"\\"+std::wstring(60,L'x');
    const auto ioFolder=fileIOPath(folder);
    require(!std::filesystem::exists(ioFolder),"Long open-folder fixture already exists.");
    for(int attempt=0;attempt<2;++attempt){
        open(folder);
        require(std::filesystem::is_directory(ioFolder) && !messages && shellCalls==1 && shellPath==folder,
                "Open folder rejected a new or existing long native path before shell dispatch.");
    }
    const std::filesystem::path blocked(fileIOPath((root/L"blocked").wstring()));
    {std::ofstream file(blocked,std::ios::binary);file<<"preserved";require(static_cast<bool>(file),"Cannot seed folder collision.");}
    open((root/L"blocked"/L"child").wstring());
    require(!shellCalls && messages==1 && lastMessage==L"The save folder is unavailable. Choose another folder.",
            "Native folder failure reached the shell or lost its diagnostic.");
    {std::ifstream file(blocked,std::ios::binary);const std::string bytes(std::istreambuf_iterator<char>(file),{});
     require(bytes=="preserved","Open folder modified the colliding file.");}
    open(folder,SE_ERR_ACCESSDENIED);
    require(shellCalls==1 && shellPath==folder && messages==1 &&
            lastMessage==L"Windows could not open the save folder. Try opening it in File Explorer:\n\n"+folder,
            "Shell failure lost the original long pathname or its separate diagnostic.");
    open(folder);
    require(shellCalls==1 && shellPath==folder && !messages && !configured,
            "Open folder failed to recover after native and shell errors.");
    std::error_code error;std::filesystem::remove_all(extendedRoot,error);
    require(!error && !std::filesystem::exists(extendedRoot),"Cannot clean the owned open-folder tree.");
    std::cout<<"PASS Open folder long Unicode paths, native and shell failures, exact spelling and retry\n";
}
}
int main(){
    try{
        fixture::HiddenWindow hidden;
        using namespace fixture;
        for(int i=0;i<3;++i){
            reset();test.create=E_OUTOFMEMORY;ordinary("activation failure",false,0);
            reset();test.show=HRESULT_FROM_WIN32(ERROR_CANCELLED);ordinary("cancel",false,1);
            reset();test.show=E_FAIL;ordinary("Show failure",false,1);
            reset();test.result=E_FAIL;ordinary("GetResult failure",false,1);
            reset();test.name=E_FAIL;ordinary("GetDisplayName failure",false,1);
            reset();test.current=HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);ordinary("initial folder absent",true,1);
            reset();test.folder=E_FAIL;ordinary("optional SetFolder failure",true,1);
            reset();test.title=E_OUTOFMEMORY;ordinary("optional title failure",true,1);
            reset();ordinary("Unicode success",true,1);
            require(requested==(FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR),"Required flags changed.");
        }
        int failures=optionsFailure(E_OUTOFMEMORY)+optionsFailure(E_FAIL)+allocationFailure(false);
        failures+=allocationFailure(true);
        reset();const int before=callbackEntries;SendMessageW(app.window,WM_COMMAND,MAKEWPARAM(Id::Folder,BN_CLICKED),0);
        require(callbackEntries==before+1 && app.settings.folder==test.selection && configured==1 && !dialogsAlive && !itemsAlive && !taskString && !messages,"Healthy callback after allocation failure did not recover.");
        std::cout<<"PASS subsequent healthy SendMessage callback; Unicode exact, live=0\n";
        openFolderLongPathsAndFailures();
        asyncFolderContracts();
        std::cout<<"failed_behavior_assertions="<<failures<<'\n';return failures?1:0;
    }catch(const std::exception& error){fault::next=false;std::cerr<<error.what()<<'\n';return 2;}
}
