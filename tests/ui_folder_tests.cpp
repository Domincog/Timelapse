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
LRESULT WINAPI message(HWND window,UINT message,WPARAM,LPARAM){
    require(reinterpret_cast<UINT_PTR>(window)>=1 && reinterpret_cast<UINT_PTR>(window)<=5,"Unexpected control handle.");
    if(message!=CB_GETCURSEL){forbidden();return 0;}return 0;
}
void WINAPI taskFree(void* memory){
    require(!memory || memory==taskString,"Unexpected task allocation.");
    if(memory)taskString=nullptr;
    CoTaskMemFree(memory);
}
int WINAPI messageBox(HWND,LPCWSTR text,LPCWSTR title,UINT flags){
    require(std::wcscmp(title,L"Timelapse")==0 && flags==(MB_OK|MB_ICONERROR),"Unexpected failure dialog options.");
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
    void refreshSources(){} void record(){} void pause(){} void setPaused(bool){} void finish(){}
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
#define WritePrivateProfileStringW fixture::writeProfile
#define GetPrivateProfileStringW fixture::readProfile
#define GetPrivateProfileIntW fixture::readInt
#include "../src/main.cpp"
#undef Engine
#undef SendMessageW
#undef CoCreateInstance
#undef SHCreateItemFromParsingName
#undef SHGetKnownFolderPath
#undef CoTaskMemFree
#undef MessageBoxW
#undef ShellExecuteW
#undef WritePrivateProfileStringW
#undef GetPrivateProfileStringW
#undef GetPrivateProfileIntW

namespace fixture {
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
    if(message==WM_COMMAND){++callbackEntries;return windowProc(window,message,wp,lp);}
    return DefWindowProcW(window,message,wp,lp);
}
struct HiddenWindow {
    const wchar_t* cls=L"OwnedNativeFolderPickerTests";
    HiddenWindow(){
        WNDCLASSW info{};info.lpfnWndProc=route;info.hInstance=GetModuleHandleW(nullptr);info.lpszClassName=cls;
        require(RegisterClassW(&info)!=0,"Cannot register owned hidden window.");
        app.window=CreateWindowExW(0,cls,L"Owned native folder picker fixture",WS_POPUP,0,0,50,50,nullptr,nullptr,info.hInstance,nullptr);
        require(app.window && !IsWindowVisible(app.window),"Cannot create hidden window.");
        app.interval=reinterpret_cast<HWND>(1);app.videoSize=reinterpret_cast<HWND>(2);app.encodingQuality=reinterpret_cast<HWND>(3);
        app.monitor=reinterpret_cast<HWND>(4);app.camera=reinterpret_cast<HWND>(5);
        app.engine=std::make_unique<lapse::FixtureEngine>();
    }
    ~HiddenWindow(){app.engine.reset();DestroyWindow(app.window);app.window=nullptr;UnregisterClassW(cls,GetModuleHandleW(nullptr));}
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
        std::cout<<"failed_behavior_assertions="<<failures<<'\n';return failures?1:0;
    }catch(const std::exception& error){fault::next=false;std::cerr<<error.what()<<'\n';return 2;}
}
