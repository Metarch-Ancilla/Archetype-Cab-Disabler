#import <AppKit/AppKit.h>
#include <csignal>
#include "../shared/wire.hpp"
#include "../../shared/channel.hpp"
#include "../shared/privilege_io.h"

static NSString *ns(const std::string &s) {
    NSString *value=[[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
    return value ?: @"";
}
static std::string bytes(NSString *s) {
    NSData *data=[s dataUsingEncoding:NSUTF8StringEncoding];
    return std::string((const char *)data.bytes,data.length);
}

// Preserve review lines; document width follows content or the viewport, whichever is wider.
@interface CabReviewText : NSTextView
@property(nonatomic) CGFloat contentWidth;
@end
@implementation CabReviewText
- (void)setFrameSize:(NSSize)size {
    NSScrollView *scroll=self.enclosingScrollView;
    size.width=MAX(self.contentWidth,scroll?scroll.contentSize.width:size.width);
    [super setFrameSize:size];
}
@end

// Disable horizontal elasticity and clamp the document to actual line widths.
@interface CabReviewScroll : NSScrollView
@property(nonatomic) BOOL sizingDocument;
@end
@implementation CabReviewScroll
- (void)tile {
    [super tile];
    if(self.sizingDocument || ![self.documentView isKindOfClass:CabReviewText.class])return;
    self.sizingDocument=YES;
    CabReviewText *text=(CabReviewText *)self.documentView;
    BOOL overflow=text.contentWidth>self.contentSize.width;
    if(self.hasHorizontalScroller!=overflow){self.hasHorizontalScroller=overflow;[super tile];}
    [text setFrameSize:NSMakeSize(MAX(text.contentWidth,self.contentSize.width),text.frame.size.height)];
    NSPoint origin=self.contentView.bounds.origin;
    origin.x=MIN(MAX(0,origin.x),MAX(0,text.frame.size.width-self.contentSize.width));
    [self.contentView scrollToPoint:origin];[self reflectScrolledClipView:self.contentView];
    self.sizingDocument=NO;
}
@end

@interface CabBoxField : NSTextField
@property(nonatomic) BOOL drawsOutline;
@end
@implementation CabBoxField
- (NSSize)intrinsicContentSize {
    NSSize size=[super intrinsicContentSize];
    // Wrapped text must fit its constrained column, never widen the window.
    size.width=NSViewNoIntrinsicMetric;return size;
}
- (void)drawRect:(NSRect)dirty {
    [super drawRect:dirty];
    if(self.drawsOutline){[NSColor.gridColor set];NSFrameRectWithWidth(NSInsetRect(self.bounds,0.5,0.5),1);}
}
@end
// Keep the read-only path's native text layout, with equal vertical padding.
@interface CabPathCell : NSTextFieldCell
@end
@implementation CabPathCell
- (NSRect)drawingRectForBounds:(NSRect)bounds {
    NSRect content=NSInsetRect(bounds,4,0);
    NSRect text=[self.stringValue boundingRectWithSize:NSMakeSize(MAX(1,content.size.width),CGFLOAT_MAX)
        options:NSStringDrawingUsesLineFragmentOrigin|NSStringDrawingUsesFontLeading
        attributes:@{NSFontAttributeName:self.font ?: [NSFont systemFontOfSize:NSFont.systemFontSize]}];
    CGFloat height=MIN(bounds.size.height,NSHeight(NSIntegralRect(text)));
    content.origin.y+=(bounds.size.height-height)/2;content.size.height=height;
    return content;
}
@end
@interface CabPathField : CabBoxField
@end
@implementation CabPathField
- (NSSize)intrinsicContentSize {
    CGFloat width=self.bounds.size.width>8?self.bounds.size.width-8:520;
    NSRect text=[self.stringValue boundingRectWithSize:NSMakeSize(width,CGFLOAT_MAX)
        options:NSStringDrawingUsesLineFragmentOrigin|NSStringDrawingUsesFontLeading
        attributes:@{NSFontAttributeName:self.font ?: [NSFont systemFontOfSize:NSFont.systemFontSize]}];
    return NSMakeSize(NSViewNoIntrinsicMetric,MAX(24,NSHeight(NSIntegralRect(text))+8));
}
- (void)setFrameSize:(NSSize)size {
    BOOL changed=self.frame.size.width!=size.width;
    [super setFrameSize:size];if(changed)[self invalidateIntrinsicContentSize];
}
@end
static NSTextField *boxedText(NSString *text, BOOL dynamicHeight=NO, BOOL boxed=YES) {
    CabBoxField *field=dynamicHeight?[[CabPathField alloc] initWithFrame:NSZeroRect]:[[CabBoxField alloc] initWithFrame:NSZeroRect];
    if(dynamicHeight)field.cell=[[CabPathCell alloc] initTextCell:@""];
    field.drawsOutline=boxed;field.stringValue=text;field.editable=NO;field.selectable=YES;field.bordered=NO;
    field.drawsBackground=boxed;field.backgroundColor=NSColor.whiteColor;field.textColor=boxed?NSColor.blackColor:NSColor.labelColor;
    field.usesSingleLineMode=NO;field.maximumNumberOfLines=0;
    field.lineBreakMode=NSLineBreakByWordWrapping;field.cell.wraps=YES;field.cell.scrollable=NO;
    [field setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    return field;
}

@interface CabCheckboxCell : NSButtonCell
@end
@implementation CabCheckboxCell
- (NSRect)checkboxFrame:(NSRect)frame {
    NSSize size=self.cellSize;
    return NSMakeRect(NSMidX(frame)-size.width/2,NSMidY(frame)-size.height/2,size.width,size.height);
}
- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view {
    [super drawWithFrame:[self checkboxFrame:frame] inView:view];
}
- (NSCellHitResult)hitTestForEvent:(NSEvent *)event inRect:(NSRect)frame ofView:(NSView *)view {
    return [super hitTestForEvent:event inRect:[self checkboxFrame:frame] ofView:view];
}
- (BOOL)trackMouse:(NSEvent *)event inRect:(NSRect)frame ofView:(NSView *)view untilMouseUp:(BOOL)untilMouseUp {
    return [super trackMouse:event inRect:[self checkboxFrame:frame] ofView:view untilMouseUp:untilMouseUp];
}
@end

@interface CabHeaderCell : NSTableHeaderCell
@property(nonatomic) NSInteger selectionState;
@end
@implementation CabHeaderCell
- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view {
    [super drawWithFrame:frame inView:view];
    NSButtonCell *check=[[CabCheckboxCell alloc] init];[check setButtonType:NSButtonTypeSwitch];
    check.title=@"";check.allowsMixedState=YES;check.state=self.selectionState;check.enabled=self.enabled;
    [check drawWithFrame:frame inView:view];
}
@end
@interface CabOutline : NSOutlineView
@end
@implementation CabOutline
- (void)drawRect:(NSRect)clip {
    [super drawRect:clip];
    [NSColor.gridColor set];
    NSRange rows=[self rowsInRect:clip];
    for(NSUInteger row=rows.location;row<NSMaxRange(rows)&&row<(NSUInteger)self.numberOfRows;++row) {
        if(row>0&&[self levelForRow:(NSInteger)row]==0) {
            NSRect rect=[self rectOfRow:(NSInteger)row];
            CGFloat scale=self.window.backingScaleFactor ?: 1;
            NSRectFill(NSMakeRect(self.bounds.origin.x,rect.origin.y,self.bounds.size.width,1/scale));
        }
    }
}
- (void)expandItem:(id)item expandChildren:(BOOL)children {
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) {
        context.duration=0;context.allowsImplicitAnimation=NO;
        [super expandItem:item expandChildren:children];
    } completionHandler:nil];
    self.needsDisplay=YES;
}
- (void)collapseItem:(id)item collapseChildren:(BOOL)children {
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) {
        context.duration=0;context.allowsImplicitAnimation=NO;
        [super collapseItem:item collapseChildren:children];
    } completionHandler:nil];
    self.needsDisplay=YES;
}
@end

@interface CabApp : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate, NSOutlineViewDataSource, NSOutlineViewDelegate> {
    nd::Model model;
    nd::Model incoming;
    bool receiving;
    nd::Lines lines;
    nd::ByteQueue backendChunks;
    nd::CommandWriter commandWriter;
    std::atomic<bool> chunkWake;
    BOOL processingChunks;
    std::vector<std::vector<size_t>> children;
    std::vector<int> states;
    NSMutableArray<NSNumber *> *treeItems;
    BOOL connected;
    NSWindow *window, *licensesWindow;
    NSStackView *form;
    NSTextField *heading, *indexing, *status, *progress;
    NSButton *back, *next;
    BOOL shown;
    NSTask *backend;
    NSPipe *input, *output;
    NSMutableDictionary<NSString *, id> *widgets;
}
@end

@implementation CabApp
- (void)alert:(NSString *)message {
    NSAlert *alert=[[NSAlert alloc] init];alert.window.appearance=[NSAppearance appearanceNamed:NSAppearanceNameAqua];alert.messageText=@"Archetype Cab Disabler";
    alert.informativeText=message;[alert runModal];
}
- (NSButton *)button:(NSString *)title action:(NSString *)action {
    NSButton *button=[NSButton buttonWithTitle:title target:self action:@selector(action:)];
    button.identifier=action;button.bezelStyle=NSBezelStyleRounded;return button;
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    widgets=[NSMutableDictionary dictionary];connected=NO;
    window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,600,460)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    window.appearance=[NSAppearance appearanceNamed:NSAppearanceNameAqua];
    window.title=@"Archetype Cab Disabler";window.minSize=NSMakeSize(560,420);window.delegate=self;
    NSStackView *outer=[NSStackView stackViewWithViews:@[]];outer.orientation=NSUserInterfaceLayoutOrientationVertical;
    outer.alignment=NSLayoutAttributeLeading;outer.spacing=8;outer.distribution=NSStackViewDistributionFill;outer.translatesAutoresizingMaskIntoConstraints=NO;
    [window.contentView addSubview:outer];
    [NSLayoutConstraint activateConstraints:@[
        [outer.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:18],
        [outer.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-18],
        [outer.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:18],
        [outer.bottomAnchor constraintEqualToAnchor:window.contentView.bottomAnchor constant:-58]]];
    heading=[NSTextField labelWithString:@""];heading.font=[NSFont boldSystemFontOfSize:15];
    indexing=[NSTextField labelWithString:@"Indexing..."];
    NSButton *licenses=[NSButton buttonWithTitle:@"Licenses" target:self action:@selector(showLicenses:)];
    licenses.bordered=NO;
    licenses.attributedTitle=[[NSAttributedString alloc] initWithString:@"Licenses" attributes:@{
        NSForegroundColorAttributeName:NSColor.linkColor,NSUnderlineStyleAttributeName:@(NSUnderlineStyleSingle),
        NSFontAttributeName:[NSFont systemFontOfSize:NSFont.systemFontSize]}];
    licenses.alignment=NSTextAlignmentRight;licenses.translatesAutoresizingMaskIntoConstraints=NO;
    [window.contentView addSubview:licenses];
    [NSLayoutConstraint activateConstraints:@[
        [licenses.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:18],
        [licenses.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-18]]];
    NSStackView *titleRow=[NSStackView stackViewWithViews:@[heading,indexing]];
    titleRow.alignment=NSLayoutAttributeFirstBaseline;titleRow.spacing=12;
    [outer addArrangedSubview:titleRow];
    [titleRow.trailingAnchor constraintLessThanOrEqualToAnchor:licenses.leadingAnchor constant:-12].active=YES;
    form=[NSStackView stackViewWithViews:@[]];form.orientation=NSUserInterfaceLayoutOrientationVertical;
    form.alignment=NSLayoutAttributeLeading;form.spacing=8;form.distribution=NSStackViewDistributionFill;form.translatesAutoresizingMaskIntoConstraints=NO;
    [outer addArrangedSubview:form];
    [form.widthAnchor constraintEqualToAnchor:outer.widthAnchor].active=YES;
    [form setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
    status=[NSTextField wrappingLabelWithString:@""];[outer addArrangedSubview:status];
    [status.widthAnchor constraintEqualToAnchor:outer.widthAnchor].active=YES;
    progress=[NSTextField labelWithString:@"Ready 0/0"];[outer addArrangedSubview:progress];
    back=[self button:@"Back" action:@"back"];
    next=[self button:@"Next" action:@"next"];next.keyEquivalent=@"\r";next.enabled=NO;
    NSView *spacer=[[NSView alloc] init];[spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *navigation=[NSStackView stackViewWithViews:@[back,spacer,next]];
    navigation.translatesAutoresizingMaskIntoConstraints=NO;[window.contentView addSubview:navigation];
    [NSLayoutConstraint activateConstraints:@[
        [navigation.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:18],
        [navigation.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-18],
        [navigation.bottomAnchor constraintEqualToAnchor:window.contentView.bottomAnchor constant:-18],
        [navigation.heightAnchor constraintEqualToConstant:32]]];
    [window center];
    NSMenu *menu=[[NSMenu alloc] init];NSMenuItem *appItem=[[NSMenuItem alloc] init];[menu addItem:appItem];
    NSMenu *appMenu=[[NSMenu alloc] init];[appMenu addItemWithTitle:@"Quit Archetype Cab Disabler" action:@selector(terminate:) keyEquivalent:@"q"];appItem.submenu=appMenu;
    NSMenuItem *editItem=[[NSMenuItem alloc] init];[menu addItem:editItem];NSMenu *edit=[[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];editItem.submenu=edit;NSApp.mainMenu=menu;
    model=nd::indexingModel();[self render];
    [self startBackend];
}
- (void)showLicenses:(id)sender {
    (void)sender;
    if(licensesWindow){[licensesWindow makeKeyAndOrderFront:nil];return;}
    NSURL *file=[NSBundle.mainBundle URLForResource:@"LICENSES" withExtension:@"txt"];
    NSError *error=nil;
    NSString *contents=file?[NSString stringWithContentsOfURL:file encoding:NSUTF8StringEncoding error:&error]:nil;
    if(!contents){[self alert:error.localizedDescription ?: @"Cannot open bundled LICENSES.txt"];return;}
    NSFont *documentFont=[NSFont userFixedPitchFontOfSize:12];
    CGFloat longest=0;
    for(NSString *line in [contents componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet])
        longest=MAX(longest,[line sizeWithAttributes:@{NSFontAttributeName:documentFont}].width);
    CGFloat contentWidth=ceil(longest+26);
    NSRect available=(window.screen ?: NSScreen.mainScreen).visibleFrame;
    licensesWindow=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,MIN(MAX(640,contentWidth+24),NSWidth(available)-36),MIN(480,NSHeight(available)-72))
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    licensesWindow.appearance=[NSAppearance appearanceNamed:NSAppearanceNameAqua];licensesWindow.title=@"Licenses";licensesWindow.releasedWhenClosed=NO;
    CabReviewScroll *scroll=[[CabReviewScroll alloc] initWithFrame:licensesWindow.contentView.bounds];
    scroll.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;scroll.hasVerticalScroller=YES;scroll.autohidesScrollers=YES;
    scroll.horizontalScrollElasticity=NSScrollElasticityNone;
    CabReviewText *text=[[CabReviewText alloc] initWithFrame:scroll.contentView.bounds];
    text.editable=NO;text.selectable=YES;text.font=documentFont;text.string=contents;
    text.textContainerInset=NSMakeSize(8,8);text.contentWidth=contentWidth;
    text.horizontallyResizable=YES;text.verticallyResizable=YES;
    text.minSize=NSZeroSize;text.maxSize=NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX);text.autoresizingMask=NSViewNotSizable;
    text.textContainer.widthTracksTextView=NO;text.textContainer.containerSize=NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX);
    scroll.documentView=text;[licensesWindow.contentView addSubview:scroll];[scroll tile];
    [licensesWindow center];[window addChildWindow:licensesWindow ordered:NSWindowAbove];
    [licensesWindow makeKeyAndOrderFront:nil];
}
- (void)startBackend {
    backend=[[NSTask alloc] init];input=[NSPipe pipe];output=[NSPipe pipe];
    backend.executableURL=[NSBundle.mainBundle.executableURL.URLByDeletingLastPathComponent URLByAppendingPathComponent:@"CabBackend"];
    backend.standardInput=input;backend.standardOutput=output;backend.standardError=[NSFileHandle fileHandleWithNullDevice];
    __weak CabApp *weakSelf=self;
    output.fileHandleForReading.readabilityHandler=^(NSFileHandle *handle) {
        NSData *data=handle.availableData;
        CabApp *app=weakSelf;if(!app)return;
        if(!data.length) {handle.readabilityHandler=nil;dispatch_async(dispatch_get_main_queue(),^{[weakSelf disconnected];});return;}
        if(!app->backendChunks.push(std::string((const char *)data.bytes,data.length))) {
            handle.readabilityHandler=nil;dispatch_async(dispatch_get_main_queue(),^{[weakSelf disconnected];});return;
        }
        if(!app->chunkWake.exchange(true))dispatch_async(dispatch_get_main_queue(),^{[weakSelf receive:nil];});
    };
    NSError *error=nil;
    if(![backend launchAndReturnError:&error]) {
        output.fileHandleForReading.readabilityHandler=nil;[self alert:error.localizedDescription];[NSApp terminate:nil];return;
    }connected=YES;
    int fd=input.fileHandleForWriting.fileDescriptor;
    int flags=fcntl(fd,F_GETFL);
    if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)){[self disconnected];return;}
    commandWriter.start([self,fd](const std::string &wire) {
        size_t offset=0;int64_t deadline=cab_now()+30000;
        while(offset<wire.size()&&!commandWriter.stopping()) {
            if(cab_now()>=deadline)return false;
            struct pollfd pipeReady={fd,POLLOUT,0};
            int ready=poll(&pipeReady,1,100);if(ready<0&&errno==EINTR)continue;
            if(ready<0)return false;if(!ready)continue;
            ssize_t n=::write(fd,wire.data()+offset,std::min<size_t>(8192,wire.size()-offset));
            if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
            if(n<=0)return false;offset+=(size_t)n;
        }
        return offset==wire.size();
    },[weakSelf] { dispatch_async(dispatch_get_main_queue(),^{[weakSelf disconnected];}); });
}
- (void)disconnected {
    connected=NO;backendChunks.close();commandWriter.close();commandWriter.join();
    @try {[input.fileHandleForWriting closeFile];} @catch(NSException *error){(void)error;}
    // Completed results and the local Finish action no longer need the backend.
    if(model.page==5&&!model.busy){next.enabled=YES;return;}
    model.busy=false;next.enabled=NO;
    if(!shown){[self alert:@"Backend disconnected before startup completed."];[NSApp terminate:nil];}
    model.status="Backend disconnected. Close and reopen the application.";[self updateStatus];
}
- (void)send:(NSString *)action fields:(const nd::Fields &)fields {
    if(!connected)return;
    if(!commandWriter.post(nd::command(bytes(action),fields),[action isEqualToString:@"cancel"]||[action isEqualToString:@"quit"]))
        [self disconnected];
}
- (void)receive:(NSData *)data {
    (void)data;
    if(processingChunks)return;
    processingChunks=YES;chunkWake=false;
    try {
        std::string chunk;
        while(backendChunks.pop(chunk,false)) {
            lines.append(chunk,[&](std::string line) {
                auto p=nd::split(line);
                if(p[0]=="ERROR")[self alert:ns(nd::unhex(p.at(1)))];
                else if(nd::receive(model,incoming,receiving,p))[self render];
                else if(p[0]=="PROGRESS"||p[0]=="STATUS")[self updateStatus];
            });
        }
    }catch(const std::exception &e) {
        backendChunks.close();commandWriter.close();receiving=false;incoming=nd::Model{};
        [self disconnected];[self alert:ns(e.what())];
    }
    processingChunks=NO;chunkWake=false;
    if(backendChunks.pending()&&!chunkWake.exchange(true))dispatch_async(dispatch_get_main_queue(),^{[self receive:nil];});
}

- (void)updateStatus {
    indexing.hidden=!model.indexing();
    status.stringValue=ns(model.status);status.hidden=model.status.empty();progress.stringValue=ns(model.progress());
    progress.hidden=!(model.busy&&model.page==4);
}
- (nd::Control *)definition:(NSString *)identifier {
    for(auto &c:model.controls)if(ns(c.id).length&&[ns(c.id) isEqualToString:identifier])return &c;
    return nullptr;
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table {
    const auto *c=[self definition:table.identifier];return c?(NSInteger)c->options.size():0;
}
- (id)tableView:(NSTableView *)table objectValueForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    const auto *c=[self definition:table.identifier];if(!c||row<0||(size_t)row>=c->options.size())return @"";
    const auto &option=c->options[(size_t)row];
    if([column.identifier isEqualToString:@"selected"])return @(option.selected);
    if([column.identifier isEqualToString:@"location"])return ns(option.location);
    return ns(option.label);
}
- (void)tableView:(NSTableView *)table setObjectValue:(id)value forTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    auto *c=[self definition:table.identifier];
    if(model.busy||!c||row<0||(size_t)row>=c->options.size()||![column.identifier isEqualToString:@"selected"])return;
    c->options[(size_t)row].selected=[value boolValue];[table reloadData];[self updateSelectAll];
}
- (NSInteger)outlineView:(NSOutlineView *)outline numberOfChildrenOfItem:(id)item {
    auto *c=[self definition:outline.identifier];if(!c)return 0;
    size_t index=item?[(NSNumber *)item unsignedIntegerValue]:c->options.size();
    return index<children.size()?(NSInteger)children[index].size():0;
}
- (id)outlineView:(NSOutlineView *)outline child:(NSInteger)index ofItem:(id)item {
    auto *c=[self definition:outline.identifier];if(!c)return nil;
    size_t parent=item?[(NSNumber *)item unsignedIntegerValue]:c->options.size();
    return treeItems[children.at(parent).at((size_t)index)];
}
- (BOOL)outlineView:(NSOutlineView *)outline isItemExpandable:(id)item {
    (void)outline;size_t index=[(NSNumber *)item unsignedIntegerValue];
    return index<children.size()&&!children[index].empty();
}
- (id)outlineView:(NSOutlineView *)outline objectValueForTableColumn:(NSTableColumn *)column byItem:(id)item {
    auto *c=[self definition:outline.identifier];size_t index=[(NSNumber *)item unsignedIntegerValue];
    if(!c||index>=c->options.size())return @"";
    if([column.identifier isEqualToString:@"selected"])
        return @(states[index]==2?NSControlStateValueOn:states[index]==1?NSControlStateValueMixed:NSControlStateValueOff);
    if([column.identifier isEqualToString:@"location"])return ns(c->options[index].location);
    return ns(c->options[index].label);
}
- (void)outlineView:(NSOutlineView *)outline setObjectValue:(id)value forTableColumn:(NSTableColumn *)column byItem:(id)item {
    (void)value;auto *c=[self definition:outline.identifier];size_t index=[(NSNumber *)item unsignedIntegerValue];
    if(model.busy||!c||index>=c->options.size()||![column.identifier isEqualToString:@"selected"])return;
    nd::toggleTree(*c,index,states[index]!=2);states=nd::treeStates(*c);[outline reloadData];[self updateSelectAll];
}
- (void)updateSelectAll {
    CabHeaderCell *button=(CabHeaderCell *)widgets[@"selectAll"];
    for(const auto &c:model.controls)if(c.kind=="plugins"||c.kind=="tree") {
        size_t count=0, checked=0;
        for(const auto &o:c.options)if(o.enabled&&(c.kind=="plugins"||o.kind=="preset")){++count;if(o.selected)++checked;}
        button.selectionState=checked==0?NSControlStateValueOff:checked==count?NSControlStateValueOn:NSControlStateValueMixed;
        button.enabled=!model.busy&&count>0;
        [(NSTableView *)widgets[ns(c.id)] headerView].needsDisplay=YES;
    }
}
- (void)tableView:(NSTableView *)table mouseDownInHeaderOfTableColumn:(NSTableColumn *)column {
    (void)table;if([column.identifier isEqualToString:@"selected"])[self selectAllRows:nil];
}
- (void)outlineView:(NSOutlineView *)outline mouseDownInHeaderOfTableColumn:(NSTableColumn *)column {
    [self tableView:outline mouseDownInHeaderOfTableColumn:column];
}
- (void)selectAllRows:(id)sender {
    if(model.busy)return;
    (void)sender;
    NSControlStateValue previous=NSControlStateValueOff;
    for(const auto &c:model.controls)if(c.kind=="plugins"||c.kind=="tree") {
        bool any=false, all=true;
        for(const auto &o:c.options)if(o.enabled&&(c.kind=="plugins"||o.kind=="preset")){any=true;all=all&&o.selected;}
        if(any&&all)previous=NSControlStateValueOn;
    }
    BOOL selected=previous!=NSControlStateValueOn;
    for(auto &c:model.controls)if(c.kind=="plugins"||c.kind=="tree") {
        for(auto &o:c.options)if(o.enabled&&(c.kind=="plugins"||o.kind=="preset"))o.selected=selected;
        if(c.kind=="tree")states=nd::treeStates(c);
        [(NSTableView *)widgets[ns(c.id)] reloadData];
    }
    [self updateSelectAll];
}
- (void)togglePresetRow:(NSOutlineView *)outline {
    NSInteger row=outline.clickedRow;
    if(row<0)return;
    id item=[outline itemAtRow:row];
    if(![outline isExpandable:item])return;
    if([outline isItemExpanded:item])[outline collapseItem:item];else [outline expandItem:item];
}
- (void)render {
    for(NSView *view in form.arrangedSubviews){[form removeArrangedSubview:view];[view removeFromSuperview];}
    [widgets removeAllObjects];
    for(const auto &c:model.controls) {
        NSString *key=ns(c.id);NSView *view=nil;
        if(!c.label.empty()&&c.kind!="buttons"&&c.kind!="radio") {
            NSTextField *label=[NSTextField wrappingLabelWithString:ns(c.label)];[form addArrangedSubview:label];
            [label.widthAnchor constraintEqualToAnchor:form.widthAnchor].active=YES;widgets[[key stringByAppendingString:@"Label"]]=label;
        }
        if(c.kind=="buttons") {
            NSMutableArray *buttons=[NSMutableArray array];
            if(!c.label.empty())[buttons addObject:[NSTextField labelWithString:ns(c.label)]];
            for(const auto &o:c.options) {
                NSButton *button=[self button:ns(o.label) action:ns(o.value)];button.enabled=!model.busy;[buttons addObject:button];
            }view=[NSStackView stackViewWithViews:buttons];widgets[key]=view;
        }else if(c.kind=="notice") {
            NSTextField *text=boxedText(ns(c.value),YES,NO);
            CGFloat side=16;
            NSImageView *icon=[NSImageView imageViewWithImage:[NSImage imageNamed:NSImageNameCaution]];
            icon.imageScaling=NSImageScaleProportionallyDown;
            NSView *notice=[[NSView alloc] init];icon.translatesAutoresizingMaskIntoConstraints=NO;text.translatesAutoresizingMaskIntoConstraints=NO;
            [notice addSubview:icon];[notice addSubview:text];
            [NSLayoutConstraint activateConstraints:@[
                [icon.leadingAnchor constraintEqualToAnchor:notice.leadingAnchor],
                [icon.centerYAnchor constraintEqualToAnchor:text.centerYAnchor],
                [icon.widthAnchor constraintEqualToConstant:side],[icon.heightAnchor constraintEqualToConstant:side],
                [text.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:8],
                [text.trailingAnchor constraintEqualToAnchor:notice.trailingAnchor],
                [text.topAnchor constraintEqualToAnchor:notice.topAnchor],
                [text.bottomAnchor constraintEqualToAnchor:notice.bottomAnchor]]];view=notice;
        }else if(c.kind=="readonly") {
            NSTextField *label=c.id=="backup_path"?boxedText(ns(c.value),YES):
                c.id=="review_summary"?boxedText(ns(c.value),YES,NO):[NSTextField wrappingLabelWithString:ns(c.value)];label.selectable=YES;
            view=label;widgets[key]=label;
        }else if(c.kind=="pair"||c.kind=="radio") {
            NSMutableArray *buttons=[NSMutableArray array];
            for(const auto &o:c.options) {
                NSButton *button=c.kind=="pair"?
                    [NSButton checkboxWithTitle:ns(o.label) target:self action:@selector(radioOption:)]:
                    [NSButton radioButtonWithTitle:ns(o.label) target:self action:@selector(radioOption:)];
                button.identifier=ns(c.id+":"+o.value);button.state=o.selected?NSControlStateValueOn:NSControlStateValueOff;
                button.enabled=!model.busy;widgets[button.identifier]=button;[buttons addObject:button];
            }
            NSStackView *row=[NSStackView stackViewWithViews:buttons];row.spacing=18;
            if(c.kind=="pair"&&c.id=="copy") {
                NSTextField *description=boxedText(@"");
                [description.heightAnchor constraintEqualToConstant:66].active=YES;
                description.font=[NSFont systemFontOfSize:11];widgets[[key stringByAppendingString:@"Description"]]=description;
                NSStackView *group=[NSStackView stackViewWithViews:@[row,description]];
                group.orientation=NSUserInterfaceLayoutOrientationVertical;group.alignment=NSLayoutAttributeLeading;group.spacing=22;view=group;
            }else if(c.kind=="pair") {
                NSView *group=[[NSView alloc] init];row.translatesAutoresizingMaskIntoConstraints=NO;
                [group addSubview:row];
                [NSLayoutConstraint activateConstraints:@[
                    [row.leadingAnchor constraintEqualToAnchor:group.leadingAnchor],
                    [row.topAnchor constraintEqualToAnchor:group.topAnchor],
                    [row.heightAnchor constraintEqualToConstant:24]]];view=group;
            }else {
                [row insertArrangedSubview:[NSTextField labelWithString:ns(c.label)] atIndex:0];view=row;
            }
        }else if(c.kind=="text") {
            NSScrollView *scroll=c.id=="report"?[[CabReviewScroll alloc] init]:[[NSScrollView alloc] init];scroll.hasVerticalScroller=YES;scroll.borderType=NSBezelBorder;
            NSTextView *text=c.id=="report"?[[CabReviewText alloc] initWithFrame:NSMakeRect(0,0,0,c.height)]:
                [[NSTextView alloc] initWithFrame:NSMakeRect(0,0,0,c.height)];
            text.editable=NO;text.selectable=YES;
            text.font=[NSFont userFixedPitchFontOfSize:12];text.string=ns(c.value);
            text.autoresizingMask=NSViewWidthSizable;text.textContainer.widthTracksTextView=YES;
            if(c.id=="report") {
                CabReviewText *review=(CabReviewText *)text;
                review.minSize=NSZeroSize;review.maxSize=NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX);
                review.horizontallyResizable=YES;review.verticallyResizable=YES;
                review.textContainer.widthTracksTextView=NO;
                review.textContainer.containerSize=NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX);
                CGFloat longest=0;
                for(NSString *line in [review.string componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet])
                    longest=MAX(longest,[line sizeWithAttributes:@{NSFontAttributeName:review.font}].width);
                review.contentWidth=ceil(longest+2*(review.textContainerInset.width+review.textContainer.lineFragmentPadding));
                review.autoresizingMask=NSViewNotSizable;
                scroll.horizontalScrollElasticity=NSScrollElasticityNone;
                scroll.autohidesScrollers=YES;
                scroll.documentView=review;[scroll tile];
            }else {
                // Results wrap to the viewport; never retain the former 900pt document width.
                text.horizontallyResizable=NO;text.verticallyResizable=YES;
                text.minSize=NSZeroSize;text.maxSize=NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX);
                scroll.hasHorizontalScroller=NO;scroll.horizontalScrollElasticity=NSScrollElasticityNone;
                scroll.autohidesScrollers=YES;scroll.documentView=text;
            }
            view=scroll;
        }else if(c.kind=="plugins"||c.kind=="tree") {
            NSScrollView *scroll=[[NSScrollView alloc] init];scroll.hasVerticalScroller=YES;scroll.hasHorizontalScroller=YES;scroll.borderType=NSBezelBorder;
            NSTableView *table;
            if(c.kind=="tree") {
                NSOutlineView *outline=[[CabOutline alloc] init];table=outline;
                children=nd::treeChildren(c);states=nd::treeStates(c);treeItems=[NSMutableArray array];
                for(size_t i=0;i<c.options.size();++i)[treeItems addObject:@(i)];
                outline.indentationPerLevel=16;outline.target=self;outline.doubleAction=@selector(togglePresetRow:);
            }else table=[[NSTableView alloc] init];
            table.identifier=key;table.dataSource=self;table.delegate=self;
            table.allowsMultipleSelection=NO;
            table.gridStyleMask=NSTableViewGridNone;
            NSTableColumn *check=[[NSTableColumn alloc] initWithIdentifier:@"selected"];check.width=28;check.minWidth=28;check.maxWidth=28;check.title=@"";
            check.headerCell=[[CabHeaderCell alloc] initTextCell:@""];widgets[@"selectAll"]=(id)check.headerCell;
            NSButtonCell *cell=[[CabCheckboxCell alloc] init];[cell setButtonType:NSButtonTypeSwitch];cell.title=@"";cell.allowsMixedState=YES;
            check.dataCell=cell;check.editable=!model.busy;[table addTableColumn:check];
            NSTableColumn *name=[[NSTableColumn alloc] initWithIdentifier:@"label"];name.width=190;name.title=c.kind=="plugins"?@"Plugin":@"Presets";name.editable=NO;
            [table addTableColumn:name];
            NSTableColumn *location=[[NSTableColumn alloc] initWithIdentifier:@"location"];location.width=1000;location.title=@"Location";location.editable=NO;
            [table addTableColumn:location];
            table.enabled=!model.busy;scroll.documentView=table;[table reloadData];
            if(c.kind=="tree") {
                NSOutlineView *outline=(NSOutlineView *)table;outline.outlineTableColumn=name;
                for(size_t i=0;i<c.options.size();++i)if(c.options[i].parent<0)[outline expandItem:treeItems[i]];
            }view=scroll;widgets[key]=table;
        }else throw std::runtime_error("Unknown native control type");
        [form addArrangedSubview:view];view.translatesAutoresizingMaskIntoConstraints=NO;
        [view.widthAnchor constraintEqualToAnchor:form.widthAnchor].active=YES;
        if(c.kind=="pair"&&c.id=="copy") {
            // Cross-view constraints require the form and field to share an ancestor.
            NSTextField *description=widgets[@"copyDescription"];
            [description.widthAnchor constraintEqualToAnchor:form.widthAnchor].active=YES;
        }
        if(c.kind=="plugins"||c.kind=="tree"||c.kind=="text") {
            [view.heightAnchor constraintGreaterThanOrEqualToConstant:100].active=YES;
            NSLayoutConstraint *height=[view.heightAnchor constraintEqualToAnchor:window.contentView.heightAnchor constant:(c.kind=="plugins"?-150:c.kind=="text"&&model.page==5?-174:-112)];
            height.priority=250;height.active=YES;
            [view setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
        }else if(c.id=="backup_path"||c.id=="review_summary"||c.kind=="notice") {
            [view.heightAnchor constraintGreaterThanOrEqualToConstant:24].active=YES;
            [view setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationVertical];
            [view setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationVertical];
        }else [view.heightAnchor constraintEqualToConstant:c.height].active=YES;
    }
    if(model.page==2||model.page==3) {
        NSView *space=[[NSView alloc] init];[space setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
        [form addArrangedSubview:space];[space.heightAnchor constraintGreaterThanOrEqualToConstant:0].active=YES;
    }
    [self updateSelectAll];[self updateBackup];[self updateEditDescription];
    heading.stringValue=ns(model.title);back.enabled=model.back;next.enabled=model.next;
    next.title=ns(model.nextLabel);[self updateStatus];
    if(!shown){shown=YES;[window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];}
    [window makeFirstResponder:next];
}
- (nd::Fields)fields {
    nd::Fields result;
    for(const auto &c:model.controls) {
        NSView *view=widgets[ns(c.id)];std::string value;
        if(c.kind=="radio"||c.kind=="pair") {
            for(const auto &o:c.options)if(((NSButton *)widgets[ns(c.id+":"+o.value)]).state==NSControlStateValueOn)value=o.value;
        }else if(!view)continue;
        else if(c.kind=="readonly")value=c.value;
        else if(c.kind=="plugins"||c.kind=="tree") {
            for(const auto &o:c.options)if(o.selected&&(c.kind=="plugins"||o.kind=="preset")) {
                if(!value.empty())value+=',';value+=o.value;
            }
        }else continue;
        result.push_back({c.id,value});
    }return result;
}
- (void)radioOption:(NSButton *)sender {
    if(model.busy)return;
    NSString *prefix=[[sender.identifier componentsSeparatedByString:@":"][0] stringByAppendingString:@":"];
    for(NSString *key in widgets)if([key hasPrefix:prefix])((NSButton *)widgets[key]).state=[key isEqualToString:sender.identifier]?NSControlStateValueOn:NSControlStateValueOff;
    [self updateBackup];
    auto *c=[self definition:[prefix substringToIndex:prefix.length-1]];
    if(c&&c->kind=="pair")for(const auto &o:c->options)if([sender.identifier isEqualToString:ns(c->id+":"+o.value)])
        c->value=o.value;
    [self updateEditDescription];
}
- (void)updateEditDescription {
    const auto *cab=[self definition:@"disable"],*mode=[self definition:@"copy"];
    if(cab&&mode)((NSTextField *)widgets[@"copyDescription"]).stringValue=ns(nd::editDescription(*cab,*mode));
}
- (void)updateBackup {
    if(model.page!=2)return;
    BOOL enabled=!model.busy&&((NSButton *)widgets[@"backup:1"]).state==NSControlStateValueOn;
    NSTextField *location=widgets[@"backup_path"];location.enabled=enabled;
    location.textColor=enabled?NSColor.blackColor:NSColor.disabledControlTextColor;
    location.selectable=enabled;
    ((NSTextField *)widgets[@"backup_pathLabel"]).textColor=enabled?NSColor.labelColor:NSColor.disabledControlTextColor;
    for(NSView *view in ((NSStackView *)widgets[@"save"]).arrangedSubviews)if([view isKindOfClass:NSButton.class])((NSButton *)view).enabled=enabled;
}
- (void)action:(NSButton *)sender {
    NSString *action=sender.identifier;
    if(model.page==5&&[action isEqualToString:@"next"]){[window performClose:nil];return;}
    if(model.busy&&![action isEqualToString:@"cancel"])return;
    nd::Fields values=[self fields];
    if([action isEqualToString:@"add"]) {
        NSOpenPanel *dialog=[NSOpenPanel openPanel];dialog.appearance=[NSAppearance appearanceNamed:NSAppearanceNameAqua];dialog.canChooseDirectories=YES;dialog.canChooseFiles=NO;dialog.allowsMultipleSelection=NO;
        if([dialog runModal]!=NSModalResponseOK)return;values.push_back({"path",bytes(dialog.URL.path)});
    }else if([action isEqualToString:@"save_backup"]) {
        if(((NSButton *)widgets[@"backup:1"]).state!=NSControlStateValueOn)return;
        NSSavePanel *dialog=[NSSavePanel savePanel];dialog.appearance=[NSAppearance appearanceNamed:NSAppearanceNameAqua];dialog.allowedFileTypes=@[@"zip"];dialog.canCreateDirectories=YES;
        NSString *initial=@"CabDisabler-backup.zip";
        for(const auto &f:values)if(f.first=="backup_path")initial=ns(f.second);
        dialog.nameFieldStringValue=initial.lastPathComponent;
        if(initial.isAbsolutePath)dialog.directoryURL=[NSURL fileURLWithPath:initial.stringByDeletingLastPathComponent];
        if([dialog runModal]!=NSModalResponseOK)return;
        if([NSFileManager.defaultManager fileExistsAtPath:dialog.URL.path]) {
            [self alert:@"Existing backups are never overwritten. Choose a new filename."];return;
        }values.push_back({"path",bytes(dialog.URL.path)});
    }
    [self send:action fields:values];
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    if(model.busy){[self send:@"cancel" fields:{}];model.status="Cancelling...";[self updateStatus];return NO;}
    [licensesWindow close];[self send:@"quit" fields:{}];return YES;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {(void)sender;return YES;}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;if(model.busy){[self send:@"cancel" fields:{}];return NSTerminateCancel;}
    [self send:@"quit" fields:{}];return NSTerminateNow;
}
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;backendChunks.close();commandWriter.close();
    output.fileHandleForReading.readabilityHandler=nil;commandWriter.join();[input.fileHandleForWriting closeFile];
}
@end

int main(int argc,const char *argv[]) {
    (void)argc;(void)argv;std::signal(SIGPIPE,SIG_IGN);
    @autoreleasepool {
        NSApplication *app=[NSApplication sharedApplication];[app setActivationPolicy:NSApplicationActivationPolicyRegular];
        __attribute__((objc_precise_lifetime)) CabApp *delegate=[[CabApp alloc] init];
        app.delegate=delegate;[app run];
    }return 0;
}
