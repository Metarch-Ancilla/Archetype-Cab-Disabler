#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <richedit.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <shlobj.h>
#include <shellapi.h>
#include "authorization.h"
#include <algorithm>
#include <memory>
#include <deque>
#include "../shared/wire.hpp"
#include "../../shared/channel.hpp"

namespace cab { int server(); }

static HWND window, panel, titleView, indexingView, licensesLink, licensesWindow, statusView, progressView, backButton, nextButton;
static HANDLE inputPipe=nullptr, outputPipe=nullptr, processHandle=nullptr, readerHandle=nullptr;
static nd::Model model;
static nd::Model incoming;
static nd::Lines backendLines;
static nd::ByteQueue backendChunks;
static nd::CommandWriter commandWriter;
static std::atomic<bool> chunkWake{false};
static bool processingChunks=false;
static bool receiving=false, shown=false, updatingSelection=false;
static int systemDpi=96;
static HFONT font=nullptr, titleFont=nullptr, linkFont=nullptr, licenseFont=nullptr;
static HMODULE richEditModule=nullptr;
struct Widget {
    nd::Control control; HWND handle=nullptr, label=nullptr, header=nullptr;
    HWND selectAll=nullptr, description=nullptr, frame=nullptr, checks=nullptr, divider=nullptr, warningIcon=nullptr;std::vector<HWND> options;
    std::vector<HTREEITEM> items;std::vector<int> states;
    int y=0,height=0;bool sizingColumns=false;
};
static std::vector<Widget> widgets;
static std::vector<std::pair<int,std::string>> actions;
static int scale(int n) { return MulDiv(n,systemDpi,96); }
static std::wstring wide(const std::string &s) {
    if(s.empty())return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0);
    if(!n)throw std::runtime_error("Invalid UTF-8 from backend");
    std::wstring out(n,0); MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),out.data(),n); return out;
}
static std::string utf8(const std::wstring &s) {
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
    std::string out(n,0); WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),out.data(),n,nullptr,nullptr); return out;
}
static std::string multiline(const std::string &s) {
    std::string out;out.reserve(s.size());
    for(size_t i=0;i<s.size();++i) {
        if(s[i]=='\n'&&(i==0||s[i-1]!='\r'))out+='\r';
        out+=s[i];
    }return out;
}
static std::wstring text(HWND h) { int n=GetWindowTextLengthW(h); std::wstring out(n+1,0); GetWindowTextW(h,out.data(),n+1);out.resize(n); return out; }
static void error(const std::string &s) { MessageBoxW(window,wide(s).c_str(),L"Archetype Cab Disabler",MB_OK|MB_ICONERROR); }
static HWND control(const wchar_t *klass,const std::string &label,DWORD style,int id,HWND parent) {
    bool edit=wcscmp(klass,L"EDIT")==0;
    auto initial=edit?std::wstring{}:wide(label);
    HWND h=CreateWindowExW(0,klass,initial.c_str(),WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|style,0,0,0,0,parent,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
    if(!h)throw std::runtime_error("Cannot create native control (Windows error " + std::to_string(GetLastError()) + ")");
    if(edit) {
        // Large review/results reports exceed the window-creation text limit.
        SendMessageW(h,EM_SETLIMITTEXT,64*1024*1024,0);
        if(!SetWindowTextW(h,wide(label).c_str())){DestroyWindow(h);throw std::runtime_error("Cannot load report text");}
    }
    SendMessageW(h,WM_SETFONT,(WPARAM)font,TRUE); return h;
}
static void drawCheckbox(HDC dc,RECT cell,int state,BOOL enabled,HTHEME theme) {
    SIZE size={scale(13),scale(13)};
    int visual=state==2?(enabled?CBS_CHECKEDNORMAL:CBS_CHECKEDDISABLED):
        state==1?(enabled?CBS_MIXEDNORMAL:CBS_MIXEDDISABLED):(enabled?CBS_UNCHECKEDNORMAL:CBS_UNCHECKEDDISABLED);
    if(theme)GetThemePartSize(theme,dc,BP_CHECKBOX,visual,nullptr,TS_TRUE,&size);
    RECT glyph={cell.left+(cell.right-cell.left-size.cx)/2,cell.top+(cell.bottom-cell.top-size.cy)/2,0,0};
    glyph.right=glyph.left+size.cx;glyph.bottom=glyph.top+size.cy;
    if(!theme||FAILED(DrawThemeBackground(theme,dc,BP_CHECKBOX,visual,&glyph,nullptr)))
        DrawFrameControl(dc,&glyph,DFC_BUTTON,(state==1?DFCS_BUTTON3STATE:DFCS_BUTTONCHECK)|(state?DFCS_CHECKED:0)|(!enabled?DFCS_INACTIVE:0));
}
// Owner drawing also covers native enable/focus/state redraws, not only WM_PAINT.
static void drawHeaderCheckbox(HWND h,HDC dc,RECT rect) {
    FillRect(dc,&rect,GetSysColorBrush(COLOR_BTNFACE));
    LRESULT checked=SendMessageW(h,BM_GETCHECK,0,0);
    HTHEME theme=OpenThemeData(h,L"Button");
    drawCheckbox(dc,rect,checked==BST_CHECKED?2:checked==BST_INDETERMINATE?1:0,IsWindowEnabled(h),theme);
    if(theme)CloseThemeData(theme);
    if((SendMessageW(h,BM_GETSTATE,0,0)&BST_FOCUS)&&!(SendMessageW(h,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)) {
        InflateRect(&rect,-1,-1);DrawFocusRect(dc,&rect);
    }
}
static LRESULT CALLBACK HeaderCheckProc(HWND h,UINT msg,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR) {
    // BS_OWNERDRAW has no native checkbox state; keep the existing message contract.
    if(msg==BM_GETCHECK)return GetWindowLongPtrW(h,GWLP_USERDATA);
    if(msg==BM_SETCHECK) {
        SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)w);InvalidateRect(h,nullptr,FALSE);return 0;
    }
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(h,HeaderCheckProc,id);
    return DefSubclassProc(h,msg,w,l);
}
static HWND tableHeader(HWND frame,const wchar_t *title,int nameWidth) {
    HWND header=control(WC_HEADERW,"",HDS_HORZ|HDS_FULLDRAG,0,frame);
    const wchar_t *names[]={L"",title,L"Location"};
    for(int i=0;i<3;++i) {
        HDITEMW column={};column.mask=HDI_TEXT|HDI_WIDTH|HDI_FORMAT;
        column.pszText=(LPWSTR)names[i];column.cxy=scale(i==0?20:i==1?nameWidth:1200);
        column.fmt=HDF_LEFT|HDF_STRING|(i==0?HDF_FIXEDWIDTH:0);Header_InsertItem(header,i,&column);
    }
    return header;
}
static void fitPluginColumns(Widget &widget,bool fillLocation=true) {
    RECT client={};GetClientRect(widget.handle,&client);
    if(widget.sizingColumns||client.right<scale(20))return;
    widget.sizingColumns=true;
    HDITEMW name={};name.mask=HDI_WIDTH;Header_GetItem(widget.header,1,&name);
    HDITEMW location={};location.mask=HDI_WIDTH;Header_GetItem(widget.header,2,&location);
    int room=(int)client.right-scale(20),nameWidth=std::clamp(name.cxy,0,room);
    int widths[]={scale(20),nameWidth,fillLocation?room-nameWidth:std::clamp(location.cxy,0,room-nameWidth)};
    for(int i=0;i<3;++i) {
        HDITEMW column={};column.mask=HDI_WIDTH;column.cxy=widths[i];Header_SetItem(widget.header,i,&column);
    }
    // Shrink before growing another column so intermediate widths cannot create horizontal overflow.
    for(int pass=0;pass<2;++pass)for(int i=0;i<3;++i) {
        int current=ListView_GetColumnWidth(widget.handle,i);
        if(pass==0?widths[i]<current:widths[i]>current)ListView_SetColumnWidth(widget.handle,i,widths[i]);
    }
    widget.sizingColumns=false;
}
static LRESULT CALLBACK PluginListProc(HWND h,UINT msg,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR) {
    LRESULT result=DefSubclassProc(h,msg,w,l);
    if(msg==WM_SIZE)for(auto &widget:widgets)if(widget.handle==h)fitPluginColumns(widget);
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(h,PluginListProc,id);
    return result;
}
static void updateTreeStates(Widget &widget) {
    auto states=nd::treeStates(widget.control);
    SetWindowRedraw(widget.handle,FALSE);
    for(size_t i=0;i<states.size();++i)if(states[i]!=widget.states[i]) {
        TVITEMW item={};item.mask=TVIF_STATE;item.hItem=widget.items[i];item.stateMask=TVIS_STATEIMAGEMASK;
        item.state=INDEXTOSTATEIMAGEMASK(states[i]==2?2:states[i]==1?3:1);TreeView_SetItem(widget.handle,&item);
    }
    widget.states=std::move(states);SetWindowRedraw(widget.handle,TRUE);
    InvalidateRect(widget.handle,nullptr,FALSE);InvalidateRect(widget.checks,nullptr,FALSE);
}
static void toggleTree(Widget &widget,size_t index) {
    if(model.busy)return;
    nd::toggleTree(widget.control,index,widget.states.at(index)!=2);updateTreeStates(widget);
}
static void updateSelectAll() {
    if(updatingSelection)return;
    for(auto &w:widgets)if(w.selectAll) {
        size_t count=0,checked=0;
        for(size_t i=0;i<w.control.options.size();++i) {
            const auto &o=w.control.options[i];
            if(!o.enabled||(w.control.kind=="tree"&&o.kind!="preset"))continue;
            ++count;if(w.control.kind=="plugins"?ListView_GetCheckState(w.handle,(int)i):o.selected)++checked;
        }
        SendMessageW(w.selectAll,BM_SETCHECK,checked==0?BST_UNCHECKED:checked==count?BST_CHECKED:BST_INDETERMINATE,0);
        EnableWindow(w.selectAll,!model.busy&&count>0);
    }
}
static void selectAllRows(HWND button) {
    if(model.busy)return;
    for(auto &w:widgets)if(w.selectAll==button) {
        bool selected=SendMessageW(button,BM_GETCHECK,0,0)!=BST_CHECKED;
        updatingSelection=true;
        if(w.control.kind=="plugins") {
            SetWindowRedraw(w.handle,FALSE);
            for(size_t i=0;i<w.control.options.size();++i)ListView_SetCheckState(w.handle,(int)i,selected);
            SetWindowRedraw(w.handle,TRUE);InvalidateRect(w.handle,nullptr,FALSE);
        }else {
            for(auto &option:w.control.options)if(option.kind=="preset"&&option.enabled)option.selected=selected;
            updateTreeStates(w);
        }
        updatingSelection=false;
    }updateSelectAll();
}
// The checkbox column stays fixed while disclosure arrows/text retain native indentation.
static LRESULT CALLBACK CheckColumnProc(HWND h,UINT msg,WPARAM w,LPARAM l) {
    for(auto &widget:widgets)if(widget.checks==h) {
        if(msg==WM_ERASEBKGND)return 1;
        if(msg==WM_PAINT) {
            PAINTSTRUCT paint;HDC target=BeginPaint(h,&paint);RECT client;GetClientRect(h,&client);
            HDC buffer=CreateCompatibleDC(target);
            HBITMAP bitmap=buffer?CreateCompatibleBitmap(target,std::max(1L,client.right),std::max(1L,client.bottom)):nullptr;
            HGDIOBJ old=bitmap?SelectObject(buffer,bitmap):nullptr;HDC dc=bitmap?buffer:target;
            FillRect(dc,&client,GetSysColorBrush(COLOR_WINDOW));
            HTHEME theme=OpenThemeData(h,L"Button");
            for(HTREEITEM item=TreeView_GetFirstVisible(widget.handle);item;item=TreeView_GetNextVisible(widget.handle,item)) {
                RECT row;TreeView_GetItemRect(widget.handle,item,&row,FALSE);if(row.top>=client.bottom)break;
                TVITEMW value={};value.mask=TVIF_PARAM;value.hItem=item;TreeView_GetItem(widget.handle,&value);
                size_t index=(size_t)value.lParam;if(index>=widget.states.size())continue;
                row.left=0;row.right=client.right;
                drawCheckbox(dc,row,widget.states[index],!model.busy,theme);
                if(index>0&&widget.control.options[index].parent<0) {
                    row.bottom=row.top+1;FillRect(dc,&row,GetSysColorBrush(COLOR_3DLIGHT));
                }
            }
            if(theme)CloseThemeData(theme);
            if(bitmap) {
                BitBlt(target,paint.rcPaint.left,paint.rcPaint.top,paint.rcPaint.right-paint.rcPaint.left,
                    paint.rcPaint.bottom-paint.rcPaint.top,dc,paint.rcPaint.left,paint.rcPaint.top,SRCCOPY);
                SelectObject(buffer,old);DeleteObject(bitmap);
            }
            if(buffer)DeleteDC(buffer);
            EndPaint(h,&paint);return 0;
        }
        if(msg==WM_LBUTTONDOWN||msg==WM_LBUTTONDBLCLK) {
            if(model.busy)return 0;
            for(HTREEITEM item=TreeView_GetFirstVisible(widget.handle);item;item=TreeView_GetNextVisible(widget.handle,item)) {
                RECT row;TreeView_GetItemRect(widget.handle,item,&row,FALSE);
                if(GET_Y_LPARAM(l)>=row.top&&GET_Y_LPARAM(l)<row.bottom) {
                    TVITEMW value={};value.mask=TVIF_PARAM;value.hItem=item;TreeView_GetItem(widget.handle,&value);
                    toggleTree(widget,(size_t)value.lParam);updateSelectAll();TreeView_SelectItem(widget.handle,item);SetFocus(widget.handle);return 0;
                }
            }
            TreeView_SelectItem(widget.handle,nullptr);SetFocus(widget.handle);return 0;
        }
    }
    return DefWindowProcW(h,msg,w,l);
}
static LRESULT CALLBACK TreeInputProc(HWND h,UINT msg,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR) {
    if(msg==WM_LBUTTONDOWN) {
        TVHITTESTINFO hit={};hit.pt={GET_X_LPARAM(l),GET_Y_LPARAM(l)};TreeView_HitTest(h,&hit);
        if(!(hit.flags&(TVHT_ONITEM|TVHT_ONITEMBUTTON))) {
            TreeView_SelectItem(h,nullptr);SetFocus(h);return 0;
        }
    }
    LRESULT result=DefSubclassProc(h,msg,w,l);
    for(auto &widget:widgets)if(widget.handle==h&&widget.checks)
        if(msg==WM_VSCROLL||msg==WM_MOUSEWHEEL||msg==WM_SIZE||msg==TVM_EXPAND) {
            if(msg==WM_SIZE) {
                RECT client;GetClientRect(h,&client);
                SetWindowPos(widget.checks,nullptr,0,0,scale(20),client.bottom,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            }
            InvalidateRect(widget.checks,nullptr,FALSE);
        }
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(h,TreeInputProc,id);
    return result;
}
static LRESULT CALLBACK TableFrameProc(HWND h,UINT msg,WPARAM w,LPARAM l) {
    if(msg==WM_NOTIFY||msg==WM_COMMAND)return SendMessageW(window,msg,w,l);
    if(msg==WM_DRAWITEM)return SendMessageW(GetParent(h),msg,w,l);
    if(msg==WM_CTLCOLORSTATIC||msg==WM_CTLCOLOREDIT) {
        SetTextColor((HDC)w,GetSysColor(COLOR_WINDOWTEXT));SetBkColor((HDC)w,RGB(255,255,255));return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    return DefWindowProcW(h,msg,w,l);
}
static LRESULT treeNotification(NMHDR *notification) {
    for(auto &widget:widgets) {
        if(widget.handle==notification->hwndFrom&&widget.control.kind=="plugins"&&notification->code==LVN_ITEMCHANGED){updateSelectAll();return 0;}
        if(widget.header==notification->hwndFrom) {
            auto change=(NMHEADERW *)notification;
            if((notification->code==HDN_BEGINTRACKW||notification->code==HDN_BEGINTRACKA||
                notification->code==HDN_DIVIDERDBLCLICKW||notification->code==HDN_DIVIDERDBLCLICKA)&&change->iItem==0)return TRUE;
            if((notification->code==HDN_ITEMCHANGINGW||notification->code==HDN_ITEMCHANGINGA)&&
               change->pitem&&(change->pitem->mask&HDI_WIDTH)&&!widget.sizingColumns) {
                if(change->iItem==0)return change->pitem->cxy!=scale(20);
                else if(widget.control.kind=="plugins") {
                    RECT client={};GetClientRect(widget.handle,&client);
                    int maximum=std::max(0,(int)client.right-scale(20));
                    if(change->iItem==2)maximum=std::max(0,maximum-ListView_GetColumnWidth(widget.handle,1));
                    change->pitem->cxy=std::clamp(change->pitem->cxy,0,maximum);
                }
            }
            if((notification->code==HDN_ITEMCHANGEDW||notification->code==HDN_ITEMCHANGEDA)&&
               change->pitem&&(change->pitem->mask&HDI_WIDTH)&&!widget.sizingColumns) {
                if(widget.control.kind=="plugins")fitPluginColumns(widget,change->iItem!=2);
                RedrawWindow(widget.handle,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);
            }
            return 0;
        }
        if(widget.handle!=notification->hwndFrom||widget.control.kind!="tree")continue;
        if(notification->code==TVN_ITEMEXPANDED)InvalidateRect(widget.checks,nullptr,FALSE);
        HTREEITEM selected=nullptr;
        if(notification->code==NM_CLICK) {
            POINT point;GetCursorPos(&point);ScreenToClient(widget.handle,&point);
            TVHITTESTINFO hit={};hit.pt=point;TreeView_HitTest(widget.handle,&hit);
            if(hit.flags&TVHT_ONITEMSTATEICON)selected=hit.hItem;
        } else if(notification->code==TVN_KEYDOWN&&((NMTVKEYDOWN*)notification)->wVKey==VK_SPACE)
            selected=TreeView_GetSelection(widget.handle);
        else if(notification->code==NM_CUSTOMDRAW) {
            auto draw=(NMTVCUSTOMDRAW*)notification;
            if(draw->nmcd.dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW;
            if(draw->nmcd.dwDrawStage==CDDS_ITEMPREPAINT)return CDRF_NOTIFYPOSTPAINT;
            if(draw->nmcd.dwDrawStage==CDDS_ITEMPOSTPAINT) {
                size_t index=(size_t)draw->nmcd.lItemlParam;
                if(index<widget.control.options.size()&&!widget.control.options[index].location.empty()) {
                    RECT row={},column={};TreeView_GetItemRect(widget.handle,(HTREEITEM)draw->nmcd.dwItemSpec,&row,FALSE);
                    Header_GetItemRect(widget.header,1,&column);POINT edge={column.right,0};MapWindowPoints(widget.header,widget.handle,&edge,1);row.left=edge.x+scale(8);
                    bool selectedRow=(draw->nmcd.uItemState&CDIS_SELECTED)!=0;
                    FillRect(draw->nmcd.hdc,&row,GetSysColorBrush(selectedRow?COLOR_HIGHLIGHT:COLOR_WINDOW));
                    SetTextColor(draw->nmcd.hdc,GetSysColor(selectedRow?COLOR_HIGHLIGHTTEXT:COLOR_WINDOWTEXT));SetBkMode(draw->nmcd.hdc,TRANSPARENT);
                    auto location=wide(widget.control.options[index].location);
                    DrawTextW(draw->nmcd.hdc,location.c_str(),-1,&row,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
                }
                if(index>0&&index<widget.control.options.size()&&widget.control.options[index].parent<0) {
                    RECT separator={};TreeView_GetItemRect(widget.handle,(HTREEITEM)draw->nmcd.dwItemSpec,&separator,FALSE);
                    RECT client={};GetClientRect(widget.handle,&client);separator.left=0;separator.right=client.right;separator.bottom=separator.top+1;
                    FillRect(draw->nmcd.hdc,&separator,GetSysColorBrush(COLOR_3DLIGHT));
                }
            }return CDRF_DODEFAULT;
        }
        if(selected) {TVITEMW item={};item.mask=TVIF_PARAM;item.hItem=selected;
            if(TreeView_GetItem(widget.handle,&item))toggleTree(widget,(size_t)item.lParam);
            updateSelectAll();return 1;
        }
    }return 0;
}
static void send(const std::string &action,const nd::Fields &fields={}) {
    if(!inputPipe)return;
    if(!commandWriter.post(nd::command(action,fields),action=="cancel"||action=="quit")) {
        PostMessageW(window,WM_APP+2,0,0);
        error("The backend connection closed or its command queue is full.");
    }
}
static nd::Fields fields() {
    nd::Fields result;
    for(const auto &w:widgets) {
        const auto &c=w.control; std::string value;
        if(c.kind=="radio"||c.kind=="pair") {
            for(size_t i=0;i<w.options.size();++i)if(SendMessageW(w.options[i],BM_GETCHECK,0,0)==BST_CHECKED)value=c.options[i].value;
        }
        else if(c.kind=="readonly")value=c.value;
        else if(c.kind=="plugins") {
            for(size_t i=0;i<c.options.size();++i)if(ListView_GetCheckState(w.handle,(int)i)) {
                if(!value.empty())value+=',';
                value+=c.options[i].value;
            }
        } else if(c.kind=="tree") {
            for(const auto &o:c.options)if(o.kind=="preset"&&o.selected) {
                if(!value.empty())value+=',';
                value+=o.value;
            }
        } else continue;
        result.push_back({c.id,value});
    } return result;
}
static std::wstring chooseFolder() {
    IFileOpenDialog *dialog=nullptr; std::wstring out;
    HRESULT hr=CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog));
    if(FAILED(hr)){error("Cannot open the folder chooser");return {};}
    DWORD options=0; dialog->GetOptions(&options); dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
    if(SUCCEEDED(dialog->Show(window))) {
        IShellItem *item=nullptr;
        if(SUCCEEDED(dialog->GetResult(&item))) { PWSTR path=nullptr;
            if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) {out=path;CoTaskMemFree(path);}item->Release(); }
    } dialog->Release();return out;
}
static std::wstring chooseFile(const std::wstring &initial) {
    IFileSaveDialog *dialog=nullptr; std::wstring out;
    if(FAILED(CoCreateInstance(CLSID_FileSaveDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) {
        error("Cannot open the save dialog");return {}; }
    COMDLG_FILTERSPEC filter={L"ZIP backup",L"*.zip"};
    dialog->SetFileTypes(1,&filter); dialog->SetDefaultExtension(L"zip");
    size_t slash=initial.find_last_of(L"\\/");
    std::wstring filename=slash==std::wstring::npos?initial:initial.substr(slash+1);
    dialog->SetFileName(filename.c_str());
    if(slash!=std::wstring::npos) { IShellItem *folder=nullptr;
        if(SUCCEEDED(SHCreateItemFromParsingName(initial.substr(0,slash).c_str(),nullptr,IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);folder->Release(); }
    }
    DWORD flags=0; dialog->GetOptions(&flags);
    dialog->SetOptions((flags|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST)&~FOS_OVERWRITEPROMPT);
    if(SUCCEEDED(dialog->Show(window))) {
        IShellItem *item=nullptr;
        if(SUCCEEDED(dialog->GetResult(&item))) {PWSTR path=nullptr;
            if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) {out=path;CoTaskMemFree(path);}item->Release();}
    }dialog->Release();
    if(!out.empty()&&GetFileAttributesW(out.c_str())!=INVALID_FILE_ATTRIBUTES) {
        error("Existing backups are never overwritten. Choose a new filename.");return {}; }
    return out;
}
static void action(const std::string &name) {
    if(model.page==5&&name=="next"){SendMessageW(window,WM_CLOSE,0,0);return;}
    if(model.busy&&name!="cancel")return;
    nd::Fields values=fields();
    if(name=="add") { auto path=chooseFolder();if(path.empty())return;values.push_back({"path",utf8(path)}); }
    if(name=="save_backup") {
        std::wstring initial=L"CabDisabler-backup.zip";
        for(auto &f:values)if(f.first=="backup_path")initial=wide(f.second);
        auto path=chooseFile(initial); if(path.empty())return; values.push_back({"path",utf8(path)});
    }
    send(name,values);
}
static LRESULT CALLBACK LicensesProc(HWND h,UINT msg,WPARAM w,LPARAM l) {
    if(msg==WM_SIZE) {
        RECT r;GetClientRect(h,&r);int pad=scale(8);
        MoveWindow(GetDlgItem(h,1),pad,pad,std::max(0,(int)r.right-2*pad),std::max(0,(int)r.bottom-2*pad),TRUE);
        return 0;
    }
    if(msg==WM_CTLCOLORSTATIC||msg==WM_CTLCOLOREDIT) {
        SetTextColor((HDC)w,RGB(0,0,0));SetBkColor((HDC)w,RGB(255,255,255));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    if(msg==WM_DESTROY){licensesWindow=nullptr;return 0;}
    return DefWindowProcW(h,msg,w,l);
}
static void showLicenses() {
    if(licensesWindow){ShowWindow(licensesWindow,SW_RESTORE);SetForegroundWindow(licensesWindow);return;}
    HMODULE module=GetModuleHandleW(nullptr);
    HRSRC resource=FindResourceW(module,MAKEINTRESOURCEW(102),RT_RCDATA);
    HGLOBAL data=resource?LoadResource(module,resource):nullptr;
    DWORD size=resource?SizeofResource(module,resource):0;
    const char *bytes=data?(const char *)LockResource(data):nullptr;
    if(!bytes||!size||size>2*1024*1024)throw std::runtime_error("Cannot read embedded licenses");
    std::string contents(bytes,size);
    if(!richEditModule)richEditModule=LoadLibraryExW(L"Msftedit.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!richEditModule)throw std::runtime_error("Cannot load Windows license viewer");
    auto value=wide(multiline(contents));
    HDC dc=GetDC(window);HGDIOBJ old=SelectObject(dc,licenseFont);int longest=0;
    for(size_t begin=0;begin<value.size();) {
        size_t end=value.find_first_of(L"\r\n",begin);if(end==std::wstring::npos)end=value.size();
        SIZE extent={};GetTextExtentPoint32W(dc,value.data()+begin,(int)(end-begin),&extent);
        longest=std::max(longest,(int)extent.cx);begin=end+1;
    }
    SelectObject(dc,old);ReleaseDC(window,dc);
    MONITORINFO monitor={};monitor.cbSize=sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);auto area=monitor.rcWork;
    RECT bounds={0,0,std::min(std::max(scale(640),longest+scale(40)),(int)(area.right-area.left)-scale(36)),
        std::min(scale(480),(int)(area.bottom-area.top)-scale(72))};
    AdjustWindowRectEx(&bounds,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_CONTROLPARENT);
    int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    licensesWindow=CreateWindowExW(WS_EX_CONTROLPARENT,L"CabLicenses",L"Licenses",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        area.left+(area.right-area.left-width)/2,area.top+(area.bottom-area.top-height)/2,width,height,
        window,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!licensesWindow)throw std::runtime_error("Cannot open licenses window");
    try {
        HWND viewer=control(MSFTEDIT_CLASS,"",WS_TABSTOP|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_AUTOHSCROLL|ES_READONLY,1,licensesWindow);
        SetWindowRedraw(viewer,FALSE);
        SendMessageW(viewer,EM_SETTEXTMODE,TM_PLAINTEXT,0);
        SendMessageW(viewer,EM_SETTARGETDEVICE,0,1); // No wrapping, including after resizing.
        SendMessageW(viewer,EM_EXLIMITTEXT,0,2*1024*1024);
        SendMessageW(viewer,WM_SETFONT,(WPARAM)licenseFont,FALSE);
        SendMessageW(viewer,EM_SETBKGNDCOLOR,0,RGB(255,255,255));
        SendMessageW(licensesWindow,WM_SIZE,0,0);
        if(!SetWindowTextW(viewer,value.c_str()))throw std::runtime_error("Cannot load license text");
        SetWindowRedraw(viewer,TRUE);ShowWindow(licensesWindow,SW_SHOWNORMAL);SetFocus(viewer);
    }catch(...){DestroyWindow(licensesWindow);throw;}

}
static void updateStatus() {
    ShowWindow(indexingView,model.indexing()?SW_SHOW:SW_HIDE);
    SetWindowTextW(statusView,wide(model.status).c_str());
    SetWindowTextW(progressView,wide(model.progress()).c_str());
    ShowWindow(progressView,(model.busy&&model.page==4)?SW_SHOW:SW_HIDE);
}
static void layout() {
    RECT r;GetClientRect(window,&r);int width=r.right,height=r.bottom,pad=scale(18),gap=scale(8),footer=scale((model.busy&&model.page==4)?100:model.status.empty()?52:78);
    HDC titleDC=GetDC(titleView);HGDIOBJ previous=SelectObject(titleDC,titleFont);
    TEXTMETRICW titleMetrics={};GetTextMetricsW(titleDC,&titleMetrics);
    SelectObject(titleDC,previous);ReleaseDC(titleView,titleDC);
    int titleHeight=std::max(scale(22),(int)(titleMetrics.tmHeight+titleMetrics.tmExternalLeading)+scale(2));
    int panelTop=scale(18)+titleHeight+gap;
    HDC headingDC=GetDC(titleView);HGDIOBJ oldHeading=SelectObject(headingDC,titleFont);
    SIZE titleSize={};auto heading=wide(model.title);
    GetTextExtentPoint32W(headingDC,heading.data(),(int)heading.size(),&titleSize);
    SelectObject(headingDC,oldHeading);ReleaseDC(titleView,headingDC);
    SIZE linkSize={};SendMessageW(licensesLink,LM_GETIDEALHEIGHT,0,(LPARAM)&linkSize);
    int linkWidth=std::max(scale(48),(int)linkSize.cx),headingWidth=width-2*pad-linkWidth-gap;
    MoveWindow(licensesLink,width-pad-linkWidth,pad,linkWidth,std::max(scale(16),(int)linkSize.cy),TRUE);
    int titleWidth=model.indexing()?std::min((int)titleSize.cx,headingWidth-scale(90)):headingWidth;
    MoveWindow(titleView,pad,scale(18),titleWidth,titleHeight,TRUE);
    HDC normalDC=GetDC(indexingView);HGDIOBJ oldNormal=SelectObject(normalDC,font);TEXTMETRICW normalMetrics={};
    GetTextMetricsW(normalDC,&normalMetrics);SelectObject(normalDC,oldNormal);ReleaseDC(indexingView,normalDC);
    MoveWindow(indexingView,pad+titleWidth+gap,scale(18)+titleMetrics.tmAscent-normalMetrics.tmAscent,
        std::max(0,headingWidth-titleWidth-gap),normalMetrics.tmHeight+scale(2),TRUE);
    MoveWindow(panel,pad,panelTop,width-2*pad,height-panelTop-footer,TRUE);
    RECT p;GetClientRect(panel,&p);int y=0,noticeHeight=0,noticeTextHeight=0;
    int iconWidth=GetSystemMetrics(SM_CXSMICON),iconHeight=GetSystemMetrics(SM_CYSMICON);
    for(const auto &widget:widgets)if(widget.control.kind=="notice") {
        HDC dc=GetDC(widget.handle);HGDIOBJ old=SelectObject(dc,font);
        RECT bounds={0,0,std::max(scale(1),(int)p.right-iconWidth-gap),0};auto value=wide(widget.control.value);
        DrawTextW(dc,value.c_str(),-1,&bounds,DT_CALCRECT|DT_WORDBREAK|DT_EDITCONTROL|DT_NOPREFIX);
        SelectObject(dc,old);ReleaseDC(widget.handle,dc);noticeTextHeight=bounds.bottom;
        noticeHeight=std::max(iconHeight,noticeTextHeight)+scale(8);
    }
    for(auto &w:widgets) {
        if(w.label&&w.control.kind!="buttons"&&w.control.kind!="radio") {MoveWindow(w.label,0,y,p.right,scale(22),TRUE);y+=scale(24);}
        w.y=y;int h=(w.control.kind=="plugins"||w.control.kind=="tree"||w.control.kind=="text")?std::max(scale(100),(int)p.bottom-y-scale(w.control.kind=="plugins"?46:8)-(w.control.kind=="text"&&noticeHeight?noticeHeight+gap:0)):scale(w.control.height);w.height=h;
        if(w.control.kind=="notice"){h=noticeHeight;w.height=h;}
        if(w.control.id=="backup_path"||w.control.id=="review_summary") {
            HDC dc=GetDC(w.handle);HGDIOBJ old=SelectObject(dc,font);
            RECT textBounds={0,0,std::max(scale(1),(int)p.right-scale(8)),0};auto value=wide(w.control.value);
            DrawTextW(dc,value.c_str(),-1,&textBounds,DT_CALCRECT|DT_WORDBREAK|DT_EDITCONTROL|DT_NOPREFIX);
            SelectObject(dc,old);ReleaseDC(w.handle,dc);h=std::max(scale(24),(int)textBounds.bottom+scale(8));w.height=h;
        }
        if(w.control.kind=="buttons") {
            int labelWidth=0;
            if(w.label) {
                HDC dc=GetDC(w.label);HGDIOBJ old=SelectObject(dc,font);SIZE measured={};
                auto label=wide(w.control.label);GetTextExtentPoint32W(dc,label.c_str(),(int)label.size(),&measured);
                SelectObject(dc,old);ReleaseDC(w.label,dc);labelWidth=measured.cx;
                MoveWindow(w.label,0,y,labelWidth,h,TRUE);
            }
            int start=w.label?labelWidth+gap:0;
            int count=(int)w.control.options.size(),bw=scale(88);
            for(int i=0;i<count;++i) { HWND button=GetDlgItem(panel,(int)(INT_PTR)w.handle+i);MoveWindow(button,start+i*(bw+gap),y,bw,h,TRUE); }
        } else if(w.control.kind=="radio"||w.control.kind=="pair") {
            int optionWidth=scale(w.control.kind=="radio"?64:180),start=w.control.kind=="radio"?scale(130):0;
            if(w.control.kind=="radio"&&w.label)MoveWindow(w.label,0,y,start-gap,scale(24),TRUE);
            for(size_t i=0;i<w.options.size();++i)MoveWindow(w.options[i],start+(int)i*optionWidth,y,optionWidth,scale(24),TRUE);
            if(w.description)MoveWindow(w.description,0,y+scale(44),p.right,h-scale(44),TRUE);
        } else if(w.control.kind=="plugins"||w.control.kind=="tree") {
            MoveWindow(w.frame,0,y,p.right,h,TRUE);
            RECT inside;GetClientRect(w.frame,&inside);
            int column=w.control.kind=="tree"?scale(20):0,header=scale(24);
            MoveWindow(w.header,0,0,inside.right,header,TRUE);
            MoveWindow(w.divider,0,header,inside.right,1,TRUE);
            MoveWindow(w.handle,column,header+1,inside.right-column,inside.bottom-header-1,TRUE);
            if(w.control.kind=="plugins")fitPluginColumns(w);
            else {
                RECT treeClient;GetClientRect(w.handle,&treeClient);
                MoveWindow(w.checks,0,header+1,column,treeClient.bottom,TRUE);
            }
        } else if(w.control.kind=="text") {
            MoveWindow(w.frame,0,y,p.right,h,TRUE);RECT inside;GetClientRect(w.frame,&inside);
            MoveWindow(w.handle,0,0,inside.right,inside.bottom,TRUE);
        } else if(w.control.kind=="notice") {
            MoveWindow(w.warningIcon,0,y+(h-iconHeight)/2,iconWidth,iconHeight,TRUE);
            MoveWindow(w.handle,iconWidth+gap,y+(h-noticeTextHeight)/2,
                       std::max(0L,p.right-iconWidth-gap),noticeTextHeight,TRUE);
        } else MoveWindow(w.handle,0,y,p.right,h,TRUE);
        if(w.selectAll) {
            RECT box={};GetClientRect(w.header,&box);
            POINT origin={0,0};MapWindowPoints(w.header,GetParent(w.selectAll),&origin,1);
            int column=scale(20);
            int height=box.bottom>1?box.bottom-1:scale(24)-1;
            MoveWindow(w.selectAll,origin.x,origin.y,std::max(1,column),height,TRUE);BringWindowToTop(w.selectAll);
        }
        y+=h+gap;
    }
    MoveWindow(statusView,pad,height-footer+gap,width-2*pad,scale(24),TRUE);
    ShowWindow(statusView,model.status.empty()?SW_HIDE:SW_SHOW);
    MoveWindow(progressView,pad,height-scale(70),width-2*pad,scale(22),TRUE);
    int by=height-scale(40),bh=scale(28),bw=scale(110);
    MoveWindow(backButton,pad,by,bw,bh,TRUE);
    MoveWindow(nextButton,width-pad-bw,by,bw,bh,TRUE);
}
static void updateBackup() {
    if(model.page!=2)return;
    bool enabled=false;
    for(const auto &w:widgets)if(w.control.id=="backup")for(size_t i=0;i<w.options.size();++i)
        if(w.control.options[i].value=="1")enabled=!model.busy&&SendMessageW(w.options[i],BM_GETCHECK,0,0)==BST_CHECKED;
    for(const auto &w:widgets)if(w.control.id=="backup_path"||w.control.id=="save") {
        if(w.label)EnableWindow(w.label,enabled);
        if(w.control.kind=="buttons")for(size_t i=0;i<w.control.options.size();++i)EnableWindow(GetDlgItem(panel,(int)(INT_PTR)w.handle+(int)i),enabled);
        else EnableWindow(w.handle,enabled);
    }
}
static void updateEditDescription() {
    const nd::Control *cab=nullptr,*mode=nullptr;HWND description=nullptr;
    for(const auto &widget:widgets) {
        if(widget.control.id=="disable")cab=&widget.control;
        if(widget.control.id=="copy"){mode=&widget.control;description=widget.description;}
    }
    if(cab&&mode&&description)SetWindowTextW(description,wide(multiline(nd::editDescription(*cab,*mode))).c_str());
}
static void render() {
    struct RedrawGuard {
        bool complete=false;
        ~RedrawGuard() {
            if(complete)return;
            SetWindowRedraw(panel,TRUE);
            EnableWindow(backButton,model.back);EnableWindow(nextButton,model.next);
            RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);
        }
    } redraw;
    SetWindowRedraw(panel,FALSE);
    HWND child=GetWindow(panel,GW_CHILD);while(child){HWND next=GetWindow(child,GW_HWNDNEXT);DestroyWindow(child);child=next;}
    widgets.clear();actions.clear();int id=100;
    widgets.reserve(model.controls.size());
    for(const auto &definition:model.controls) {
        Widget w;w.control=definition;const auto &c=w.control;
        if(!c.label.empty())w.label=control(L"STATIC",c.label,SS_LEFT|(c.kind=="buttons"?SS_CENTERIMAGE:0),0,panel);
        if(c.kind=="plugins"||c.kind=="tree") {
            w.frame=control(L"CabTable","",WS_BORDER|WS_CLIPCHILDREN,0,panel);
            w.selectAll=control(L"BUTTON","",BS_OWNERDRAW|WS_TABSTOP,id++,w.frame?w.frame:panel);
            SetWindowSubclass(w.selectAll,HeaderCheckProc,1,0);
        }
        if(c.kind=="buttons") {
            w.handle=(HWND)(INT_PTR)id;
            for(const auto &o:c.options) {HWND b=control(L"BUTTON",o.label,BS_PUSHBUTTON|WS_TABSTOP,id,panel);EnableWindow(b,!model.busy);actions.push_back({id++,o.value});}
        } else {
            DWORD style=WS_TABSTOP;
            if(c.kind=="text") {
                w.frame=control(L"CabTable","",WS_BORDER|WS_CLIPCHILDREN,0,panel);
                w.handle=control(L"EDIT",multiline(c.value),style|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,id++,w.frame);
            }
            else if(c.kind=="notice") {
                w.warningIcon=control(L"STATIC","",SS_ICON|SS_REALSIZECONTROL,0,panel);
                SendMessageW(w.warningIcon,STM_SETICON,(WPARAM)LoadImageW(nullptr,IDI_WARNING,IMAGE_ICON,
                    GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED),0);
                w.handle=control(L"STATIC",multiline(c.value),SS_LEFT|SS_EDITCONTROL,id++,panel);
            }
            else if(c.kind=="readonly")w.handle=c.id=="backup_path"?
                control(L"EDIT",multiline(c.value),WS_BORDER|ES_MULTILINE|ES_READONLY,id++,panel):
                control(L"STATIC",multiline(c.value),SS_LEFT|SS_EDITCONTROL,id++,panel);
            else if(c.kind=="radio"||c.kind=="pair") {
                for(const auto &o:c.options) {
                    HWND option=control(L"BUTTON",o.label,(c.kind=="pair"?BS_CHECKBOX:BS_RADIOBUTTON)|WS_TABSTOP,id++,panel);
                    SendMessageW(option,BM_SETCHECK,o.selected?BST_CHECKED:BST_UNCHECKED,0);
                    EnableWindow(option,!model.busy);w.options.push_back(option);
                }w.handle=w.options.at(0);
                if(c.kind=="pair"&&c.id=="copy")
                    w.description=control(L"EDIT","",WS_BORDER|ES_MULTILINE|ES_READONLY,0,panel);
            } else if(c.kind=="plugins") {
                w.header=tableHeader(w.frame,L"Plugin",192);
                w.divider=control(L"STATIC","",SS_ETCHEDHORZ,0,w.frame);
                w.handle=control(WC_LISTVIEWW,"",style|WS_CLIPCHILDREN|LVS_REPORT|LVS_NOCOLUMNHEADER|LVS_SINGLESEL|LVS_SHOWSELALWAYS,id++,w.frame);
                SetWindowSubclass(w.handle,PluginListProc,1,0);
                ListView_SetExtendedListViewStyle(w.handle,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
                const wchar_t *names[]={L"",L"Plugin",L"Location"};
                for(int i=0;i<3;++i){LVCOLUMNW column={};column.mask=LVCF_TEXT|LVCF_WIDTH;column.pszText=(LPWSTR)names[i];column.cx=scale(i==0?20:i==1?192:900);ListView_InsertColumn(w.handle,i,&column);}
                for(size_t i=0;i<c.options.size();++i) {
                    LVITEMW item={};item.mask=LVIF_TEXT;item.iItem=(int)i;item.pszText=(LPWSTR)L"";ListView_InsertItem(w.handle,&item);
                    auto name=wide(c.options[i].label),path=wide(c.options[i].location);
                    ListView_SetItemText(w.handle,(int)i,1,name.data());ListView_SetItemText(w.handle,(int)i,2,path.data());
                    ListView_SetCheckState(w.handle,(int)i,c.options[i].selected);
                }
            } else if(c.kind=="tree") {
                w.header=tableHeader(w.frame,L"Presets",240);
                w.divider=control(L"STATIC","",SS_ETCHEDHORZ,0,w.frame);
                w.handle=control(WC_TREEVIEWW,"",style|TVS_NOHSCROLL|TVS_HASBUTTONS|TVS_HASLINES|TVS_LINESATROOT|TVS_SHOWSELALWAYS,id++,w.frame);
                SendMessageW(w.handle,TVM_SETEXTENDEDSTYLE,TVS_EX_DOUBLEBUFFER,TVS_EX_DOUBLEBUFFER);
                w.checks=control(L"CabChecks","",0,0,w.frame);
                SetWindowSubclass(w.handle,TreeInputProc,1,0);w.states=nd::treeStates(w.control);
                for(size_t i=0;i<c.options.size();++i) {
                    TVINSERTSTRUCTW item={};item.hParent=c.options[i].parent<0?TVI_ROOT:w.items.at((size_t)c.options[i].parent);item.hInsertAfter=TVI_LAST;
                    item.item.mask=TVIF_TEXT|TVIF_PARAM|TVIF_STATE;auto label=wide(c.options[i].label);item.item.pszText=label.data();item.item.lParam=(LPARAM)i;
                    item.item.stateMask=TVIS_STATEIMAGEMASK;item.item.state=INDEXTOSTATEIMAGEMASK(w.states[i]==2?2:w.states[i]==1?3:1);
                    w.items.push_back(TreeView_InsertItem(w.handle,&item));
                }
                for(size_t i=0;i<c.options.size();++i)if(c.options[i].parent<0)TreeView_Expand(w.handle,w.items[i],TVE_EXPAND);
            } else throw std::runtime_error("Unknown native control type");
            EnableWindow(w.handle,!model.busy||c.kind=="text");
        }
        widgets.push_back(std::move(w));
    }
    SetWindowTextW(titleView,wide(model.title).c_str());
    EnableWindow(backButton,model.back);EnableWindow(nextButton,model.next);
    SetWindowTextW(nextButton,wide(model.nextLabel).c_str());updateStatus();updateEditDescription();layout();
    SetWindowRedraw(panel,TRUE);
    // Rebuilding the page leaves every new child dirty, not just its parent.
    RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW);
    updateSelectAll();updateBackup();
    if(!shown){shown=true;ShowWindow(window,SW_SHOWNORMAL);}
    if(!licensesWindow||GetForegroundWindow()!=licensesWindow)SetFocus(nextButton);
    redraw.complete=true;
}
// The pipe reader transfers chunks to the UI thread; only that thread updates controls.
static DWORD WINAPI readBackend(void *) {
    char buffer[8192];DWORD n;
    while(ReadFile(outputPipe,buffer,sizeof(buffer),&n,nullptr)&&n) {
        if(!backendChunks.push(std::string(buffer,n)))break;
        if(!chunkWake.exchange(true)&&!PostMessageW(window,WM_APP+1,0,0))break;
    }
    PostMessageW(window,WM_APP+2,0,0);return 0;
}
static void startBackend() {
    std::wstring path(32768,0);DWORD n=GetModuleFileNameW(nullptr,path.data(),(DWORD)path.size());
    if(!n||n>=path.size())throw std::runtime_error("Cannot locate application");
    path.resize(n);

    SECURITY_ATTRIBUTES sa={sizeof(sa),nullptr,TRUE};HANDLE childIn=nullptr,childOut=nullptr;
    if(!CreatePipe(&childIn,&inputPipe,&sa,0)||!CreatePipe(&outputPipe,&childOut,&sa,0))throw std::runtime_error("Cannot create backend pipes");
    SetHandleInformation(inputPipe,HANDLE_FLAG_INHERIT,0);SetHandleInformation(outputPipe,HANDLE_FLAG_INHERIT,0);
    HANDLE err=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
    STARTUPINFOEXW si={};si.StartupInfo.cb=sizeof(si);si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput=childIn;si.StartupInfo.hStdOutput=childOut;si.StartupInfo.hStdError=err;
    SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);std::vector<unsigned char> attrs(size);
    si.lpAttributeList=(LPPROC_THREAD_ATTRIBUTE_LIST)attrs.data();
    HANDLE inherited[]={childIn,childOut,err};
    if(!InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&size)||
       !UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr))throw std::runtime_error("Cannot isolate backend handles");
    PROCESS_INFORMATION pi={};std::wstring cmd=L"\""+path+L"\" --backend";
    BOOL ok=CreateProcessW(path.c_str(),cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);CloseHandle(childIn);CloseHandle(childOut);CloseHandle(err);
    if(!ok)throw std::runtime_error("Cannot start preset backend mode.");
    processHandle=pi.hProcess;CloseHandle(pi.hThread);
    commandWriter.start([](const std::string &out) {
        size_t offset=0;
        while(offset<out.size()&&!commandWriter.stopping()) {
            DWORD written=0;
            if(!WriteFile(inputPipe,out.data()+offset,(DWORD)std::min<size_t>(8192,out.size()-offset),&written,nullptr)||!written) {
                PostMessageW(window,WM_APP+2,0,0);return false;
            }
            offset+=written;
        }
        return offset==out.size();
    });
    readerHandle=CreateThread(nullptr,0,readBackend,nullptr,0,nullptr);
    if(!readerHandle)throw std::runtime_error("Cannot read backend connection");
}
static LRESULT CALLBACK PanelProc(HWND h,UINT msg,WPARAM w,LPARAM l) {
    if(msg==WM_DRAWITEM) {
        auto draw=(DRAWITEMSTRUCT *)l;
        for(const auto &widget:widgets)if(widget.selectAll==draw->hwndItem) {
            drawHeaderCheckbox(draw->hwndItem,draw->hDC,draw->rcItem);return TRUE;
        }
    }
    if(msg==WM_CTLCOLORSTATIC||msg==WM_CTLCOLOREDIT)for(const auto &widget:widgets)if((widget.control.id=="copy"&&widget.description==(HWND)l)||(widget.control.id=="backup_path"&&widget.handle==(HWND)l)||(widget.control.kind=="text"&&widget.handle==(HWND)l)) {
        SetTextColor((HDC)w,IsWindowEnabled((HWND)l)?RGB(0,0,0):GetSysColor(COLOR_GRAYTEXT));SetBkColor((HDC)w,RGB(255,255,255));return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    if(msg==WM_COMMAND||msg==WM_NOTIFY)return SendMessageW(window,msg,w,l);
    return DefWindowProcW(h,msg,w,l);
}
static LRESULT CALLBACK WindowProc(HWND h,UINT msg,WPARAM w,LPARAM l) {
    try {
        if(msg==WM_SIZE&&panel){layout();return 0;}
        if(msg==WM_GETMINMAXINFO) {
            auto p=(MINMAXINFO*)l;RECT minimum={0,0,scale(560),scale(420)};
            AdjustWindowRectEx(&minimum,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_CONTROLPARENT);
            p->ptMinTrackSize={minimum.right-minimum.left,minimum.bottom-minimum.top};return 0;
        }
        if(msg==WM_NOTIFY) {
            auto notice=(NMHDR*)l;
            if(notice->hwndFrom==licensesLink&&(notice->code==NM_CLICK||notice->code==NM_RETURN)){showLicenses();return 0;}
            return treeNotification(notice);
        }
        if(msg==WM_COMMAND&&HIWORD(w)==BN_CLICKED) {
            int id=LOWORD(w);
            for(auto &widget:widgets) {
                if(widget.selectAll==(HWND)l){selectAllRows((HWND)l);return 0;}
                if(widget.control.kind=="radio"||widget.control.kind=="pair")for(size_t i=0;i<widget.options.size();++i)if(widget.options[i]==(HWND)l) {
                    HWND option=widget.options[i];
                    for(HWND other:widget.options)SendMessageW(other,BM_SETCHECK,other==option?BST_CHECKED:BST_UNCHECKED,0);
                    updateBackup();
                    widget.control.value=widget.control.options[i].value;
                    updateEditDescription();
                    return 0;
                }
            }
            if(id==1)action("back");else if(id==2)action("next");
            else {for(auto &a:actions)if(a.first==id){action(a.second);break;}}
            return 0;
        }
        if(msg==WM_APP+1) {
            // Native modal dialogs can dispatch more pipe messages reentrantly.
            if(processingChunks)return 0;
            processingChunks=true;chunkWake=false;
            try {
                std::string chunk;
                while(backendChunks.pop(chunk,false)) {
                    backendLines.append(chunk,[&](std::string line) {
                        auto p=nd::split(line);
                        if(p[0]=="ERROR")error(nd::unhex(p.at(1)));
                        else if(nd::receive(model,incoming,receiving,p))render();
                        else if(p[0]=="PROGRESS"||p[0]=="STATUS")updateStatus();
                    });
                }
            }catch(...) {
                processingChunks=false;backendChunks.close();commandWriter.close();commandWriter.interrupt();
                model.busy=false;model.next=false;receiving=false;incoming=nd::Model{};
                PostMessageW(window,WM_APP+2,0,0);throw;
            }
            processingChunks=false;chunkWake=false;
            if(backendChunks.pending()&&!chunkWake.exchange(true))PostMessageW(window,WM_APP+1,0,0);
            return 0;
        }
        if(msg==WM_APP+2) {
            backendChunks.close();commandWriter.close();commandWriter.interrupt();commandWriter.join();
            if(inputPipe){CloseHandle(inputPipe);inputPipe=nullptr;}
            // Completed results and the local Finish action no longer need the backend.
            if(model.page==5&&!model.busy){EnableWindow(nextButton,TRUE);return 0;}
            if(!shown){error("Backend disconnected before startup completed.");DestroyWindow(h);return 0;}model.busy=false;EnableWindow(nextButton,FALSE);model.status="Backend disconnected. Close and reopen the application.";updateStatus();layout();return 0;}
        if(msg==WM_CLOSE) {if(model.busy){send("cancel");model.status="Cancelling...";updateStatus();return 0;}send("quit");DestroyWindow(h);return 0;}
        if(msg==WM_DESTROY){PostQuitMessage(0);return 0;}
    }catch(const std::exception &e){
        SetWindowRedraw(panel,TRUE);
        EnableWindow(backButton,model.back);EnableWindow(nextButton,model.next);
        RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
        error(e.what());
    }
    return DefWindowProcW(h,msg,w,l);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    (void)show;
    // Dispatch before creating any GUI, COM dialog or backend process.
    int argc=0;wchar_t **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv)return 1;
    if(argc>1) {
        int result=1;
        try {
            if(argc==2&&wcscmp(argv[1],L"--backend")==0)result=cab::server();
            else if(wcscmp(argv[1],L"--privileged-helper")==0)result=cab_privileged_main(argc,argv);
        }catch(const std::exception &){result=2;}
        LocalFree(argv);return result;
    }
    LocalFree(argv);
    INITCOMMONCONTROLSEX common={sizeof(common),ICC_LISTVIEW_CLASSES|ICC_TREEVIEW_CLASSES|ICC_LINK_CLASS};InitCommonControlsEx(&common);
    SetProcessDPIAware();HDC screen=GetDC(nullptr);if(screen){systemDpi=GetDeviceCaps(screen,LOGPIXELSX);ReleaseDC(nullptr,screen);}
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    NONCLIENTMETRICSW metrics={};metrics.cbSize=sizeof(metrics);SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0);font=CreateFontIndirectW(&metrics.lfMessageFont);
    WNDCLASSW wc={};wc.hInstance=instance;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(101));
    wc.lpfnWndProc=PanelProc;wc.lpszClassName=L"CabPanel";RegisterClassW(&wc);
    wc.lpfnWndProc=TableFrameProc;wc.lpszClassName=L"CabTable";RegisterClassW(&wc);
    wc.lpfnWndProc=CheckColumnProc;wc.lpszClassName=L"CabChecks";RegisterClassW(&wc);
    wc.lpfnWndProc=LicensesProc;wc.lpszClassName=L"CabLicenses";RegisterClassW(&wc);
    wc.lpfnWndProc=WindowProc;wc.lpszClassName=L"CabWindow";RegisterClassW(&wc);
    RECT bounds={0,0,scale(600),scale(460)},work={};
    AdjustWindowRectEx(&bounds,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_CONTROLPARENT);
    SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    window=CreateWindowExW(WS_EX_CONTROLPARENT,L"CabWindow",L"Archetype Cab Disabler",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,width,height,nullptr,nullptr,instance,nullptr);
    try {
        titleView=control(L"STATIC","",0,0,window);
        indexingView=control(L"STATIC","Indexing...",0,0,window);
        licensesLink=control(WC_LINK,"<a>Licenses</a>",WS_TABSTOP,3,window);
        LOGFONTW linkMetrics=metrics.lfMessageFont;linkMetrics.lfHeight=-scale(13);
        linkFont=CreateFontIndirectW(&linkMetrics);SendMessageW(licensesLink,WM_SETFONT,(WPARAM)linkFont,TRUE);
        LOGFONTW mono=metrics.lfMessageFont;mono.lfHeight=-scale(12);mono.lfPitchAndFamily=FIXED_PITCH|FF_MODERN;
        lstrcpyW(mono.lfFaceName,L"Consolas");licenseFont=CreateFontIndirectW(&mono);
        metrics.lfMessageFont.lfWeight=FW_SEMIBOLD;metrics.lfMessageFont.lfHeight=-scale(18);
        titleFont=CreateFontIndirectW(&metrics.lfMessageFont);SendMessageW(titleView,WM_SETFONT,(WPARAM)titleFont,TRUE);
        panel=CreateWindowExW(WS_EX_CONTROLPARENT,L"CabPanel",L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,0,0,1,1,window,nullptr,instance,nullptr);
        statusView=control(L"STATIC","",0,0,window);progressView=control(L"STATIC","Ready 0/0",0,0,window);
        backButton=control(L"BUTTON","Back",WS_TABSTOP,1,window);
        nextButton=control(L"BUTTON","Next",BS_DEFPUSHBUTTON|WS_TABSTOP,2,window);
        model=nd::indexingModel();render();startBackend();
        MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0)if(!(licensesWindow&&IsDialogMessageW(licensesWindow,&msg))&&!IsDialogMessageW(window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    }catch(const std::exception &e){error(e.what());}
    backendChunks.close();commandWriter.close();
    commandWriter.interrupt();
    commandWriter.join();
    if(inputPipe)CloseHandle(inputPipe);
    if(readerHandle){CancelSynchronousIo(readerHandle);WaitForSingleObject(readerHandle,5000);CloseHandle(readerHandle);}
    if(outputPipe)CloseHandle(outputPipe);
    if(processHandle)CloseHandle(processHandle);
    if(licensesWindow)DestroyWindow(licensesWindow);
    if(richEditModule)FreeLibrary(richEditModule);
    if(licenseFont)DeleteObject(licenseFont);
    if(linkFont)DeleteObject(linkFont);
    if(titleFont)DeleteObject(titleFont);
    if(font)DeleteObject(font);
    CoUninitialize();return 0;
}
