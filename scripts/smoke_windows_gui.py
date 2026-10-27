"""Smoke the compiled Win32 wizard/packaged backend under Wine using synthetic presets."""
import argparse, ctypes, json, os, subprocess, tempfile, time
from ctypes import wintypes as w
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('--root',type=Path,required=True);parser.add_argument('--report',type=Path,required=True);parser.add_argument('--capture-dir',type=Path);args=parser.parse_args()
user=ctypes.WinDLL('user32',use_last_error=True)
CALLBACK=ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
user.EnumWindows.argtypes=(CALLBACK,w.LPARAM);user.EnumChildWindows.argtypes=(w.HWND,CALLBACK,w.LPARAM)
user.GetWindowThreadProcessId.argtypes=(w.HWND,ctypes.POINTER(w.DWORD))
user.GetClassNameW.argtypes=(w.HWND,w.LPWSTR,ctypes.c_int)
user.GetWindowTextW.argtypes=(w.HWND,w.LPWSTR,ctypes.c_int)
user.SendMessageW.argtypes=(w.HWND,w.UINT,w.WPARAM,w.LPARAM);user.SendMessageW.restype=w.LPARAM
user.GetDlgCtrlID.argtypes=(w.HWND,);user.IsWindowVisible.argtypes=(w.HWND,);user.IsWindowEnabled.argtypes=(w.HWND,)
user.GetWindowRect.argtypes=(w.HWND,ctypes.POINTER(w.RECT))
user.GetWindowLongPtrW.argtypes=(w.HWND,ctypes.c_int);user.GetWindowLongPtrW.restype=w.LPARAM
user.GetClientRect.argtypes=(w.HWND,ctypes.POINTER(w.RECT))
class HeaderItem(ctypes.Structure):
    _fields_=[('mask',w.UINT),('width',ctypes.c_int),('text',ctypes.c_void_p),('bitmap',w.HBITMAP),
              ('textLength',ctypes.c_int),('format',ctypes.c_int),('data',w.LPARAM),
              ('image',ctypes.c_int),('order',ctypes.c_int),('type',w.UINT),
              ('filter',ctypes.c_void_p),('state',w.UINT)]
def header_column(header,index,width=None):
    # Header messages carry pointers: use a buffer in our own test app, not this driver process.
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.OpenProcess.argtypes=(w.DWORD,w.BOOL,w.DWORD);kernel.OpenProcess.restype=w.HANDLE
    kernel.VirtualAllocEx.argtypes=(w.HANDLE,ctypes.c_void_p,ctypes.c_size_t,w.DWORD,w.DWORD);kernel.VirtualAllocEx.restype=ctypes.c_void_p
    transfer=(w.HANDLE,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t))
    kernel.WriteProcessMemory.argtypes=transfer;kernel.ReadProcessMemory.argtypes=transfer
    kernel.VirtualFreeEx.argtypes=(w.HANDLE,ctypes.c_void_p,ctypes.c_size_t,w.DWORD)
    kernel.CloseHandle.argtypes=(w.HANDLE,)
    owner=w.DWORD();user.GetWindowThreadProcessId(header,ctypes.byref(owner))
    assert owner.value==process.pid
    handle=kernel.OpenProcess(0x38,False,owner.value);assert handle
    memory=None
    try:
        item=HeaderItem();item.mask=1 if width is not None else 5
        if width is not None:item.width=width
        memory=kernel.VirtualAllocEx(handle,None,ctypes.sizeof(item),0x3000,4);assert memory
        assert kernel.WriteProcessMemory(handle,memory,ctypes.byref(item),ctypes.sizeof(item),None)
        accepted=user.SendMessageW(header,0x120C if width is not None else 0x120B,index,memory)
        if width is None:assert accepted
        assert kernel.ReadProcessMemory(handle,memory,ctypes.byref(item),ctypes.sizeof(item),None)
        return item
    finally:
        if memory:kernel.VirtualFreeEx(handle,memory,0,0x8000)
        kernel.CloseHandle(handle)
def visible_header(window):
    return wait(lambda: next((h for h in children(window) if klass(h)=='SysHeader32' and user.IsWindowVisible(h)), None), 'visible table header')
def fixed_checkbox_column(header):
    original=header_column(header,0)
    assert original.format&0x100, 'Checkbox header must be fixed width'
    header_column(header,0,original.width+60)
    assert header_column(header,0).width==original.width, 'Checkbox width changed'
    assert user.GetWindowLongPtrW(header,-16)&0x80, 'Header must live-update while dragging'
def label(handle):
    text=ctypes.create_unicode_buffer(8192);user.SendMessageW(handle,0x000D,len(text),ctypes.addressof(text));return text.value
def klass(handle):
    text=ctypes.create_unicode_buffer(128);user.GetClassNameW(handle,text,len(text));return text.value
def children(handle):
    values=[]
    @CALLBACK
    def collect(child,_):values.append(child);return True
    user.EnumChildWindows(handle,collect,0);return values
def find(handle,kind,text=None,identifier=None):
    for child in children(handle):
        if klass(child)==kind and (text is None or label(child)==text) and (identifier is None or user.GetDlgCtrlID(child)==identifier):return child
    return None
def wait(condition,description):
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        value=condition()
        if value:return value
        time.sleep(.03)
    
    @CALLBACK
    def debug(h,_):
        pid=w.DWORD();user.GetWindowThreadProcessId(h,ctypes.byref(pid))
        if pid.value==process.pid:print('WINDOW',klass(h),repr(label(h)),bool(user.IsWindowVisible(h)),[(klass(c),label(c)) for c in children(h)],flush=True)
        return True
    user.EnumWindows(debug,0)
    print('PROCESS',process.poll(),flush=True)
    raise AssertionError('Timed out: '+description)
def click(handle):
    assert handle and user.IsWindowEnabled(handle)
    user.SendMessageW(handle,0x00F5,0,0) # BM_CLICK
import struct

def capture(handle,name):
    if args.capture_dir is None:return
    args.capture_dir.mkdir(parents=True,exist_ok=True)
    # Capture the existing screen pixels; do not force WM_PRINT/repaint.
    time.sleep(.15)
    gdi=ctypes.WinDLL('gdi32')
    user.GetDC.argtypes=(w.HWND,);user.GetDC.restype=w.HDC
    user.ReleaseDC.argtypes=(w.HWND,w.HDC)
    gdi.CreateCompatibleDC.argtypes=(w.HDC,);gdi.CreateCompatibleDC.restype=w.HDC
    gdi.CreateDIBSection.argtypes=(w.HDC,ctypes.c_void_p,w.UINT,ctypes.POINTER(ctypes.c_void_p),w.HANDLE,w.DWORD);gdi.CreateDIBSection.restype=w.HBITMAP
    gdi.SelectObject.argtypes=(w.HDC,w.HANDLE);gdi.SelectObject.restype=w.HANDLE
    gdi.BitBlt.argtypes=(w.HDC,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,w.HDC,ctypes.c_int,ctypes.c_int,w.DWORD)
    gdi.DeleteObject.argtypes=(w.HANDLE,);gdi.DeleteDC.argtypes=(w.HDC,)
    r=w.RECT();user.GetWindowRect(handle,ctypes.byref(r));width=r.right-r.left;height=r.bottom-r.top
    header=struct.pack('<IiiHHIIiiII',40,width,-height,1,32,0,width*height*4,0,0,0,0)
    info=ctypes.create_string_buffer(header);pixels=ctypes.c_void_p()
    screen=user.GetDC(None);dc=gdi.CreateCompatibleDC(screen)
    bitmap=gdi.CreateDIBSection(screen,info,0,ctypes.byref(pixels),None,0);assert bitmap
    old=gdi.SelectObject(dc,bitmap);assert gdi.BitBlt(dc,0,0,width,height,screen,r.left,r.top,0x00CC0020)
    payload=ctypes.string_at(pixels,width*height*4)
    (args.capture_dir/(name+'.bmp')).write_bytes(struct.pack('<2sIHHI',b'BM',54+len(payload),0,0,54)+header+payload)
    gdi.SelectObject(dc,old);gdi.DeleteObject(bitmap);gdi.DeleteDC(dc);user.ReleaseDC(None,screen)

with tempfile.TemporaryDirectory(prefix='NeuralDSP-GUI-') as directory:
    data=Path(directory);library=data/'Neural DSP/Parity Plugin/Presets'
    source=library/'Factory/Tone.xml';source.parent.mkdir(parents=True)
    original=b'<x cabActive="1"/>';source.write_bytes(original)
    own=library/'User/Own.xml';own.parent.mkdir();own.write_bytes(original)
    env=dict(os.environ,PROGRAMDATA=str(data))
    process=subprocess.Popen([str(args.root.resolve()/'Archetype Cab Disabler.exe')],env=env)
    window=None
    try:
        def locate():
            found=[]
            @CALLBACK
            def collect(handle,_):
                pid=w.DWORD();user.GetWindowThreadProcessId(handle,ctypes.byref(pid))
                if pid.value==process.pid and klass(handle)=='CabWindow' and user.IsWindowVisible(handle):found.append(handle)
                return True
            user.EnumWindows(collect,0);return found[0] if found else None
        window=wait(locate,'ready main window')
        def page(name):
            value=wait(lambda:find(window,'Static',name),'page '+name)
            wait(lambda:user.IsWindowEnabled(find(window,'Button',identifier=2)),'ready page '+name)
            panel=find(window,'CabPanel');assert panel
            assert not user.GetWindowLongPtrW(panel,-16)&0x00300000,'Outer page must not have scrollbars'
            capture(window,name.replace(' ','-'))
            return value
        page('Select Plugins')
        plugins=find(window,'SysListView32')
        header=visible_header(window);fixed_checkbox_column(header)
        assert user.GetWindowLongPtrW(plugins,-16)&0x4000, 'Built-in list header must be hidden'
        header_rect=w.RECT();body_rect=w.RECT()
        user.GetWindowRect(header,ctypes.byref(header_rect));user.GetWindowRect(plugins,ctypes.byref(body_rect))
        assert body_rect.top>header_rect.bottom, 'Plugin scrolling must start below the header'
        original_width=header_column(header,1).width
        header_column(header,1,100000)
        assert not user.GetWindowLongPtrW(plugins,-16)&0x00100000, 'Column drag caused horizontal overflow'
        header_column(header,1,original_width)
        assert header_column(header,1).width==original_width
        user.SendMessageW(plugins,0x0114,1,0) # WM_HSCROLL / SB_LINERIGHT
        assert not user.GetWindowLongPtrW(plugins,-16)&0x00100000,'plugin horizontal scrollbar removed'
        initial=w.RECT();user.GetWindowRect(window,ctypes.byref(initial))
        click(find(window,'Button',identifier=100))
        assert user.SendMessageW(find(window,'Button',identifier=100),0x00F0,0,0)==1
        click(find(window,'Button','Next'));page('Select presets')
        tree=find(window,'SysTreeView32');assert tree
        header=visible_header(window);fixed_checkbox_column(header)
        assert not user.GetWindowLongPtrW(tree,-16)&0x00100000,'preset horizontal scrollbar removed'
        root=user.SendMessageW(tree,0x110A,0,0)
        child=user.SendMessageW(tree,0x110A,4,root)
        user.SendMessageW(tree,0x1102,2,child)
        # A fixed native-themed column keeps every level aligned with the header.
        user_folder=user.SendMessageW(tree,0x110A,1,child)
        checks=find(window,'CabChecks');assert checks
        item_height=user.SendMessageW(tree,0x111C,0,0)
        assert (user.SendMessageW(tree,0x1127,user_folder,0xF000)>>12)==1
        for expected in (2,1):
            point=10|(3*item_height+item_height//2)<<16
            user.SendMessageW(checks,0x201,1,point);user.SendMessageW(checks,0x202,0,point)
            assert (user.SendMessageW(tree,0x1127,user_folder,0xF000)>>12)==expected
        # Blank space clears the visual caret without changing checkbox selection.
        assert user.SendMessageW(tree,0x110A,9,0)
        rect=w.RECT();user.GetWindowRect(tree,ctypes.byref(rect))
        user.SendMessageW(tree,0x201,1,5|((rect.bottom-rect.top-25)<<16))
        user.SendMessageW(tree,0x202,0,5|((rect.bottom-rect.top-25)<<16))
        assert not user.SendMessageW(tree,0x110A,9,0)
        capture(window,'Expanded-presets')
        click(find(window,'Button','Next'));page('Backup')
        click(find(window,'Button','No'))
        location=next(h for h in children(window) if label(h).endswith('.zip'))
        assert not user.IsWindowEnabled(location) and not user.IsWindowEnabled(find(window,'Button','Browse…'))
        click(find(window,'Button','Next'));page('Edit mode')
        current=w.RECT();user.GetWindowRect(window,ctypes.byref(current))
        assert current.right-current.left==initial.right-initial.left
        description=find(window,'Edit')
        assert label(description).splitlines()==[
            'Disable cab simulation in the selected presets.',
            'Modify the saved presets directly, maintaining their names and locations within the preset browser.',
            'Direct edit may require elevated permissions.']
        assert not find(window,'Static','Disable cab simulation in the selected presets.')
        click(find(window,'Button','Enable'))
        assert label(description).splitlines()[0]=='Enable cab simulation in the selected presets.'
        copy_button=find(window,'Button','Copy and Edit');click(copy_button)
        assert len(label(description).splitlines())==2 and 'elevated permissions' not in label(description)
        click(find(window,'Button','Disable'))
        assert label(description).splitlines()[0]=='Disable cab simulation in the selected presets.'
        button_rect=w.RECT();description_rect=w.RECT()
        user.GetWindowRect(copy_button,ctypes.byref(button_rect));user.GetWindowRect(description,ctypes.byref(description_rect))
        assert description_rect.top-button_rect.bottom>=16, 'Missing blank line above description box'
        click(find(window,'Button','Next'));page('Review')
        apply=wait(lambda:find(window,'Button','Apply'),'Apply label')
        wait(lambda:user.IsWindowEnabled(apply),'Apply ready')
        notice=next(h for h in children(window) if label(h)=='Backup not enabled. Proceed at your own risk.')
        assert any(klass(h)=='Static' and user.GetWindowLongPtrW(h,-16)&0x1F==0x03 for h in children(window)), 'Missing native warning icon'
        notice_rect=w.RECT();next_rect=w.RECT();user.GetWindowRect(notice,ctypes.byref(notice_rect));user.GetWindowRect(apply,ctypes.byref(next_rect))
        assert notice_rect.bottom<=next_rect.top
        summary=next(h for h in children(window) if klass(h)=='Static' and label(h).startswith('Mode:'))
        listing=find(window,'Edit');review=label(listing)
        assert not user.GetWindowLongPtrW(listing,-16)&0x00100000, 'Review must not scroll horizontally'
        assert 'Operation: Disable' in label(summary) and 'Presets: 1' in label(summary)
        assert 'Mode:' not in review and 'Backup:' not in review and 'Tone.xml' not in review and str(library) not in review
        summary_rect=w.RECT();listing_rect=w.RECT()
        user.GetWindowRect(summary,ctypes.byref(summary_rect));user.GetWindowRect(listing,ctypes.byref(listing_rect))
        assert summary_rect.bottom<=listing_rect.top, 'Summary must sit above the preset box'
        assert not user.GetWindowLongPtrW(summary,-16)&0x00800000, 'Summary must be unboxed'
        click(apply);page('Results')
        finish=wait(lambda:find(window,'Button','Finish'),'Finish')
        assert find(window,'Static','Errors:')
        assert any('Successful: 1' in label(h) and 'Failed: 0' in label(h) for h in children(window))
        assert source.read_bytes()==original and own.read_bytes()==original
        assert (library/'Cab Disabled/Factory/Tone.xml').read_bytes()==b'<x cabActive="0"/>'
        assert not (library/'Cab Disabled/User/Own.xml').exists()
        click(finish);process.wait(timeout=10);assert process.returncode==0
    finally:
        if process.poll() is None:process.terminate();process.wait(timeout=10)
report=dict(compiled_win32_wizard_and_packaged_backend=True,header_selection_persists=True,
            plugin_tree=True,native_checkbox_mouse_toggling=True,backup_no_disables_readonly_path_and_browse=True,window_width_stable_on_edit_page=True,
            no_backup_notice_above_navigation=True,relative_review_names=True,checked_leaf_copy_only=True,
            results_counts=True,finish_closes=True,scope='Win32 executable plus packaged backend under Wine; native Windows/UAC UI remains unverified')
args.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
