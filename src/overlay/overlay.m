// overlay.m - In-game console overlay implementation
// NSWindow-based floating console with Tanit symbol

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include "overlay.h"
#include "../core/logging.h"

// Forward declarations
@class BG3SEOverlayWindow;
@class BG3SEOverlayView;
@class BG3SEConsoleView;
@class BG3SESettingsPanel;

// ============================================================================
// Static State
// ============================================================================

static BG3SEOverlayWindow *s_overlay_window = nil;
static BG3SEConsoleView *s_console_view = nil;
static BG3SESettingsPanel *s_settings_panel = nil;
static overlay_settings_callback s_settings_callback = NULL;
static overlay_command_callback s_command_callback = NULL;
static bool s_initialized = false;

// Colors - Aldea Tanit warm amber/gold palette
#define OVERLAY_BG_COLOR [NSColor colorWithRed:0.08 green:0.08 blue:0.10 alpha:0.94]
#define OVERLAY_BORDER_COLOR [NSColor colorWithRed:0.984 green:0.749 blue:0.141 alpha:0.6]
// Tanit colors: warm gold matching rgba(253,224,71) and rgba(251,191,36)
#define TANIT_PRIMARY [NSColor colorWithRed:0.992 green:0.878 blue:0.278 alpha:1.0]    // #FDE047
#define TANIT_SECONDARY [NSColor colorWithRed:0.984 green:0.749 blue:0.141 alpha:1.0]  // #FBBF24
#define TANIT_GLOW [NSColor colorWithRed:0.984 green:0.749 blue:0.141 alpha:0.4]
#define TEXT_COLOR [NSColor colorWithRed:1.0 green:1.0 blue:1.0 alpha:1.0]  // Pure white for legibility
#define DIM_TEXT_COLOR [NSColor colorWithRed:0.7 green:0.7 blue:0.7 alpha:1.0]  // Dimmer for brackets
#define INPUT_BG_COLOR [NSColor colorWithRed:0.12 green:0.12 blue:0.14 alpha:1.0]
#define CLOSE_BUTTON_COLOR [NSColor colorWithRed:0.6 green:0.6 blue:0.6 alpha:1.0]
#define CLOSE_BUTTON_HOVER [NSColor colorWithRed:0.9 green:0.3 blue:0.3 alpha:1.0]

// Dimensions
#define OVERLAY_WIDTH 800
#define OVERLAY_HEIGHT 480
#define TANIT_SIZE 36
#define BORDER_WIDTH 2
#define PADDING 16
#define FONT_SIZE 13
#define INPUT_HEIGHT 32
#define TAB_HEIGHT 28
#define TAB_WIDTH 80

// Tab indices
typedef enum {
    TAB_CONSOLE = 0,
    TAB_MODS = 1,
    TAB_ENTITIES = 2,
    TAB_COUNT = 3
} ConsoleTab;

// ============================================================================
// Tanit Symbol View - Loads PNG from assets
// ============================================================================

@interface BG3SETanitView : NSImageView
@end

@implementation BG3SETanitView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;
        self.imageScaling = NSImageScaleProportionallyUpOrDown;
        self.imageAlignment = NSImageAlignCenter;

        // Load the Tanit PNG from the dylib bundle or executable path
        [self loadTanitImage];

        // Add subtle pulsing glow animation
        [self addGlowAnimation];
    }
    return self;
}

- (void)loadTanitImage {
    // Try multiple paths to find the Tanit PNG
    NSArray *searchPaths = @[
        // Relative to executable
        [[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent],
        // Development path
        @"/Users/tomdimino/Desktop/Programming/bg3se-macos/assets",
        // Alongside the dylib
        @".",
    ];

    for (NSString *basePath in searchPaths) {
        NSString *imagePath = [basePath stringByAppendingPathComponent:@"tanit.png"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:imagePath]) {
            NSImage *image = [[NSImage alloc] initWithContentsOfFile:imagePath];
            if (image) {
                self.image = image;
                return;
            }
        }
    }

    // Fallback: create a simple gold circle if PNG not found
    NSImage *fallback = [[NSImage alloc] initWithSize:NSMakeSize(64, 64)];
    [fallback lockFocus];
    [[NSColor colorWithRed:0.992 green:0.878 blue:0.278 alpha:1.0] setFill];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(8, 8, 48, 48)] fill];
    [fallback unlockFocus];
    self.image = fallback;
}

- (void)addGlowAnimation {
    // Pulsing opacity animation for subtle glow effect
    CABasicAnimation *pulseAnim = [CABasicAnimation animationWithKeyPath:@"opacity"];
    pulseAnim.fromValue = @0.85;
    pulseAnim.toValue = @1.0;
    pulseAnim.duration = 2.0;
    pulseAnim.autoreverses = YES;
    pulseAnim.repeatCount = HUGE_VALF;
    pulseAnim.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
    [self.layer addAnimation:pulseAnim forKey:@"pulse"];
}

- (BOOL)isFlipped {
    return YES;
}

@end

// ============================================================================
// Close Button - X button to close the console
// ============================================================================

@interface BG3SECloseButton : NSButton
@property (nonatomic, assign) BOOL isHovered;
@end

@implementation BG3SECloseButton

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        // Use NSButtonTypeMomentaryChange for custom drawn buttons
        [self setButtonType:NSButtonTypeMomentaryChange];
        self.bordered = NO;
        self.wantsLayer = YES;
        self.layer.cornerRadius = frame.size.width / 2;
        self.title = @"";
        self.imagePosition = NSNoImage;
        _isHovered = NO;

        // Track mouse for hover effect
        NSTrackingArea *trackingArea = [[NSTrackingArea alloc]
            initWithRect:self.bounds
            options:(NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways)
            owner:self
            userInfo:nil];
        [self addTrackingArea:trackingArea];
    }
    return self;
}

- (void)drawRect:(NSRect)dirtyRect {
    CGFloat w = self.bounds.size.width;
    CGFloat h = self.bounds.size.height;

    // Background circle on hover
    if (_isHovered) {
        [[NSColor colorWithRed:0.3 green:0.3 blue:0.3 alpha:0.8] setFill];
        [[NSBezierPath bezierPathWithOvalInRect:self.bounds] fill];
    }

    // Draw X
    NSColor *xColor = _isHovered ? CLOSE_BUTTON_HOVER : CLOSE_BUTTON_COLOR;
    [xColor setStroke];

    NSBezierPath *path = [NSBezierPath bezierPath];
    path.lineWidth = 2.0;
    path.lineCapStyle = NSLineCapStyleRound;

    CGFloat inset = 6;
    [path moveToPoint:NSMakePoint(inset, inset)];
    [path lineToPoint:NSMakePoint(w - inset, h - inset)];
    [path moveToPoint:NSMakePoint(w - inset, inset)];
    [path lineToPoint:NSMakePoint(inset, h - inset)];
    [path stroke];
}

- (void)mouseEntered:(NSEvent *)event {
    _isHovered = YES;
    [self setNeedsDisplay:YES];
}

- (void)mouseExited:(NSEvent *)event {
    _isHovered = NO;
    [self setNeedsDisplay:YES];
}

@end

// ============================================================================
// Tab Button - Individual tab in the tab bar
// ============================================================================

@interface BG3SETabButton : NSButton
@property (nonatomic, assign) BOOL isSelected;
@property (nonatomic, assign) ConsoleTab tabIndex;
@end

@implementation BG3SETabButton

- (instancetype)initWithFrame:(NSRect)frame title:(NSString *)title tabIndex:(ConsoleTab)idx {
    self = [super initWithFrame:frame];
    if (self) {
        // Use NSButtonTypeMomentaryChange for custom drawn buttons
        [self setButtonType:NSButtonTypeMomentaryChange];
        self.title = title;
        self.bordered = NO;
        self.wantsLayer = YES;
        self.imagePosition = NSNoImage;
        _tabIndex = idx;
        _isSelected = NO;
        self.font = [NSFont systemFontOfSize:11 weight:NSFontWeightMedium];
    }
    return self;
}

- (void)drawRect:(NSRect)dirtyRect {
    if (_isSelected) {
        // Selected tab - gold underline
        [[NSColor colorWithRed:0.15 green:0.15 blue:0.17 alpha:1.0] setFill];
        NSRectFill(self.bounds);

        // Gold underline
        [TANIT_PRIMARY setFill];
        NSRectFill(NSMakeRect(0, self.bounds.size.height - 2, self.bounds.size.width, 2));
    }

    // Draw title
    NSColor *textColor = _isSelected ? TANIT_PRIMARY : [NSColor colorWithRed:0.6 green:0.6 blue:0.6 alpha:1.0];
    NSDictionary *attrs = @{
        NSFontAttributeName: self.font,
        NSForegroundColorAttributeName: textColor
    };
    NSSize textSize = [self.title sizeWithAttributes:attrs];
    CGFloat x = (self.bounds.size.width - textSize.width) / 2;
    CGFloat y = (self.bounds.size.height - textSize.height) / 2;
    [self.title drawAtPoint:NSMakePoint(x, y) withAttributes:attrs];
}

- (void)setIsSelected:(BOOL)isSelected {
    _isSelected = isSelected;
    [self setNeedsDisplay:YES];
}

// Accept clicks even when the overlay window is not key (first-click activation)
- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    return YES;
}

@end

// ============================================================================
// Log Level Colors - For syntax highlighting in output
// ============================================================================

static NSColor* colorForLogLevel(const char* text) {
    if (!text) return TEXT_COLOR;

    // Error patterns
    if (strstr(text, "[ERROR]") || strstr(text, "Error:") || strstr(text, "error:") ||
        strstr(text, "FAILED") || strstr(text, "Exception")) {
        return [NSColor colorWithRed:1.0 green:0.4 blue:0.4 alpha:1.0];  // Red
    }
    // Warning patterns
    if (strstr(text, "[WARN]") || strstr(text, "Warning:") || strstr(text, "warning:")) {
        return [NSColor colorWithRed:1.0 green:0.8 blue:0.3 alpha:1.0];  // Yellow/amber
    }
    // Success patterns
    if (strstr(text, "[OK]") || strstr(text, "Success") || strstr(text, "Loaded") ||
        strstr(text, "initialized") || strstr(text, "enabled")) {
        return [NSColor colorWithRed:0.4 green:0.9 blue:0.5 alpha:1.0];  // Green
    }
    // Debug/trace patterns
    if (strstr(text, "[DEBUG]") || strstr(text, "[TRACE]") || strstr(text, "-->")) {
        return [NSColor colorWithRed:0.5 green:0.7 blue:0.9 alpha:1.0];  // Light blue
    }
    // Entity/test tags
    if (strstr(text, "[EntityTest]") || strstr(text, "[StaticData]")) {
        return [NSColor colorWithRed:0.7 green:0.6 blue:0.9 alpha:1.0];  // Purple
    }

    return TEXT_COLOR;
}

// ============================================================================
// Console View - Input field + Output text area with tabs
// ============================================================================

@interface BG3SEConsoleView : NSView <NSTextFieldDelegate>
@property (nonatomic, strong) NSScrollView *scrollView;
@property (nonatomic, strong) NSTextView *outputView;
@property (nonatomic, strong) NSTextField *inputField;
@property (nonatomic, strong) NSTextField *promptLabel;
@property (nonatomic, strong) BG3SETanitView *tanitView;
@property (nonatomic, strong) BG3SECloseButton *closeButton;
@property (nonatomic, strong) NSMutableArray<NSString *> *commandHistory;
@property (nonatomic, assign) NSInteger historyIndex;
// Tab system
@property (nonatomic, strong) NSView *tabBar;
@property (nonatomic, strong) NSMutableArray<BG3SETabButton *> *tabButtons;
@property (nonatomic, assign) ConsoleTab currentTab;
// Mods view
@property (nonatomic, strong) NSScrollView *modsScrollView;
@property (nonatomic, strong) NSView *modsContentView;
// Entities view (placeholder)
@property (nonatomic, strong) NSView *entitiesView;
// Input area container (for hiding on non-console tabs)
@property (nonatomic, strong) NSView *inputArea;
// Output coalescing (appendOutput: may be called in bursts from any thread)
// Ordered stream of pending console output: NSString lines plus NSNull
// clear sentinels (see clearOutput) — drained in order by flushPendingOutput.
@property (nonatomic, strong) NSMutableArray *pendingLines;
@property (nonatomic, assign) BOOL flushScheduled;
@end

@implementation BG3SEConsoleView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;
        self.layer.backgroundColor = OVERLAY_BG_COLOR.CGColor;
        self.layer.borderColor = OVERLAY_BORDER_COLOR.CGColor;
        self.layer.borderWidth = BORDER_WIDTH;
        self.layer.cornerRadius = 8;

        _commandHistory = [NSMutableArray array];
        _historyIndex = -1;

        [self setupSubviews];
    }
    return self;
}

- (void)setupSubviews {
    CGFloat w = self.bounds.size.width;
    CGFloat h = self.bounds.size.height;

    // ========== Header Area ==========

    // Tanit symbol in top-left corner
    _tanitView = [[BG3SETanitView alloc] initWithFrame:NSMakeRect(PADDING, PADDING, TANIT_SIZE, TANIT_SIZE)];
    [self addSubview:_tanitView];

    // Title label next to Tanit - vertically centered with symbol
    CGFloat titleY = PADDING + (TANIT_SIZE - 18) / 2;
    NSTextField *titleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(PADDING + TANIT_SIZE + 10, titleY, 200, 18)];
    titleLabel.stringValue = @"BG3SE Console";
    titleLabel.font = [NSFont boldSystemFontOfSize:15];
    titleLabel.textColor = TANIT_PRIMARY;
    titleLabel.backgroundColor = [NSColor clearColor];
    titleLabel.bordered = NO;
    titleLabel.editable = NO;
    titleLabel.selectable = NO;
    [self addSubview:titleLabel];

    // Close button (X) in top-right corner
    CGFloat closeButtonSize = 24;
    _closeButton = [[BG3SECloseButton alloc] initWithFrame:NSMakeRect(w - PADDING - closeButtonSize, PADDING + (TANIT_SIZE - closeButtonSize) / 2, closeButtonSize, closeButtonSize)];
    _closeButton.target = self;
    _closeButton.action = @selector(closeButtonClicked:);
    [self addSubview:_closeButton];

    // ========== Tab Bar ==========

    CGFloat tabBarY = PADDING + TANIT_SIZE + 8;
    _tabBar = [[NSView alloc] initWithFrame:NSMakeRect(PADDING, tabBarY, w - PADDING * 2, TAB_HEIGHT)];
    _tabBar.wantsLayer = YES;
    _tabBar.layer.backgroundColor = [NSColor colorWithRed:0.1 green:0.1 blue:0.12 alpha:1.0].CGColor;

    _tabButtons = [NSMutableArray array];
    NSArray *tabTitles = @[@"Console", @"Mods", @"Entities"];
    for (int i = 0; i < TAB_COUNT; i++) {
        BG3SETabButton *tab = [[BG3SETabButton alloc] initWithFrame:NSMakeRect(i * TAB_WIDTH, 0, TAB_WIDTH, TAB_HEIGHT)
                                                               title:tabTitles[i]
                                                            tabIndex:i];
        tab.target = self;
        tab.action = @selector(tabClicked:);
        [_tabBar addSubview:tab];
        [_tabButtons addObject:tab];
    }
    _currentTab = TAB_CONSOLE;
    _tabButtons[TAB_CONSOLE].isSelected = YES;
    [self addSubview:_tabBar];

    // ========== Content Area (below tabs) ==========

    CGFloat contentTop = tabBarY + TAB_HEIGHT + 8;
    CGFloat inputAreaHeight = INPUT_HEIGHT + PADDING;
    CGFloat contentHeight = h - contentTop - inputAreaHeight;

    // --- Console Tab: Output view ---
    _scrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(PADDING, contentTop, w - PADDING * 2, contentHeight)];
    _scrollView.hasVerticalScroller = YES;
    _scrollView.hasHorizontalScroller = NO;
    _scrollView.autohidesScrollers = YES;
    _scrollView.borderType = NSNoBorder;
    _scrollView.backgroundColor = [NSColor clearColor];
    _scrollView.drawsBackground = NO;

    _outputView = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, w - PADDING * 2 - 15, contentHeight)];
    // Force TextKit 1: TextKit 2's synchronizeTextLayoutManagers SIGBUSes on
    // rapid append+scroll bursts (2026-07-28 crash, PID 2556, tier-2 output).
    (void)_outputView.layoutManager;
    _outputView.backgroundColor = [NSColor clearColor];
    _outputView.drawsBackground = NO;
    _outputView.textColor = TEXT_COLOR;
    _outputView.font = [NSFont fontWithName:@"Menlo" size:FONT_SIZE];
    _outputView.editable = NO;
    _outputView.selectable = YES;
    _outputView.textContainerInset = NSMakeSize(4, 4);
    [_outputView setAllowsUndo:NO];
    [_outputView setRichText:YES];  // Enable rich text for colored output
    [_outputView setImportsGraphics:NO];
    [_outputView setAutoresizingMask:NSViewWidthSizable];

    _scrollView.documentView = _outputView;
    [self addSubview:_scrollView];

    // --- Mods Tab: Mods list view (initially hidden) ---
    _modsScrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(PADDING, contentTop, w - PADDING * 2, contentHeight)];
    _modsScrollView.hasVerticalScroller = YES;
    _modsScrollView.hasHorizontalScroller = NO;
    _modsScrollView.autohidesScrollers = YES;
    _modsScrollView.borderType = NSNoBorder;
    _modsScrollView.backgroundColor = [NSColor clearColor];
    _modsScrollView.drawsBackground = NO;
    _modsScrollView.hidden = YES;

    _modsContentView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, w - PADDING * 2 - 15, contentHeight)];
    _modsContentView.wantsLayer = YES;
    _modsScrollView.documentView = _modsContentView;
    [self addSubview:_modsScrollView];

    // --- Entities Tab: Placeholder (initially hidden) ---
    _entitiesView = [[NSView alloc] initWithFrame:NSMakeRect(PADDING, contentTop, w - PADDING * 2, contentHeight)];
    _entitiesView.wantsLayer = YES;
    _entitiesView.hidden = YES;

    // Placeholder label for entities
    NSTextField *entitiesLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, contentHeight / 2, 300, 40)];
    entitiesLabel.stringValue = @"Entity Browser\n(Coming Soon)";
    entitiesLabel.font = [NSFont systemFontOfSize:16 weight:NSFontWeightLight];
    entitiesLabel.textColor = [NSColor colorWithRed:0.5 green:0.5 blue:0.5 alpha:1.0];
    entitiesLabel.backgroundColor = [NSColor clearColor];
    entitiesLabel.bordered = NO;
    entitiesLabel.editable = NO;
    entitiesLabel.alignment = NSTextAlignmentCenter;
    [_entitiesView addSubview:entitiesLabel];
    [self addSubview:_entitiesView];

    // ========== Input Area (for Console tab) ==========

    CGFloat inputY = h - PADDING - INPUT_HEIGHT;
    _inputArea = [[NSView alloc] initWithFrame:NSMakeRect(0, inputY - 4, w, INPUT_HEIGHT + 8)];

    CGFloat promptWidth = 20;

    // Prompt label ">"
    _promptLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(PADDING, 4, promptWidth, INPUT_HEIGHT)];
    _promptLabel.stringValue = @">";
    _promptLabel.font = [NSFont fontWithName:@"Menlo-Bold" size:FONT_SIZE];
    _promptLabel.textColor = TANIT_PRIMARY;
    _promptLabel.backgroundColor = [NSColor clearColor];
    _promptLabel.bordered = NO;
    _promptLabel.editable = NO;
    _promptLabel.selectable = NO;
    NSTextFieldCell *promptCell = _promptLabel.cell;
    [promptCell setLineBreakMode:NSLineBreakByClipping];
    [_inputArea addSubview:_promptLabel];

    // Input field
    _inputField = [[NSTextField alloc] initWithFrame:NSMakeRect(PADDING + promptWidth, 4, w - PADDING * 2 - promptWidth, INPUT_HEIGHT)];
    _inputField.font = [NSFont fontWithName:@"Menlo" size:FONT_SIZE];
    _inputField.textColor = TEXT_COLOR;
    _inputField.backgroundColor = INPUT_BG_COLOR;
    _inputField.bordered = NO;
    _inputField.focusRingType = NSFocusRingTypeNone;
    _inputField.placeholderString = @"Enter Lua command...";
    _inputField.delegate = self;
    _inputField.wantsLayer = YES;
    _inputField.layer.cornerRadius = 4;
    _inputField.allowsEditingTextAttributes = NO;
    [[_inputField cell] setUsesSingleLineMode:YES];
    [[_inputField cell] setScrollable:YES];
    [_inputArea addSubview:_inputField];

    [self addSubview:_inputArea];

    // Initialize mods list with placeholder data
    [self updateModsList];
}

// Tab switching - with safety checks to prevent crashes
- (void)tabClicked:(id)sender {
    @try {
        // Safety: verify sender is valid
        if (!sender || ![sender isKindOfClass:[BG3SETabButton class]]) {
            NSLog(@"[BG3SE Console] tabClicked: invalid sender");
            return;
        }

        BG3SETabButton *tabButton = (BG3SETabButton *)sender;
        ConsoleTab newTab = tabButton.tabIndex;

        // Safety: bounds check
        if (newTab < 0 || newTab >= TAB_COUNT) {
            NSLog(@"[BG3SE Console] tabClicked: invalid tab index %d", (int)newTab);
            return;
        }

        if (_currentTab == newTab) return;

        // Safety: verify array exists and indices are valid
        if (!_tabButtons || _tabButtons.count == 0) {
            NSLog(@"[BG3SE Console] tabClicked: tabButtons not initialized");
            return;
        }

        if (_currentTab >= (ConsoleTab)_tabButtons.count || newTab >= (ConsoleTab)_tabButtons.count) {
            NSLog(@"[BG3SE Console] tabClicked: index out of bounds");
            return;
        }

        // Deselect old tab
        _tabButtons[_currentTab].isSelected = NO;

        // Select new tab
        _currentTab = newTab;
        tabButton.isSelected = YES;

        // Defer view changes to next run loop iteration to avoid issues during event handling
        dispatch_async(dispatch_get_main_queue(), ^{
            @try {
                // Show/hide views based on tab - with nil checks
                if (self->_scrollView) self->_scrollView.hidden = (self->_currentTab != TAB_CONSOLE);
                if (self->_modsScrollView) self->_modsScrollView.hidden = (self->_currentTab != TAB_MODS);
                if (self->_entitiesView) self->_entitiesView.hidden = (self->_currentTab != TAB_ENTITIES);
                if (self->_inputArea) self->_inputArea.hidden = (self->_currentTab != TAB_CONSOLE);

                if (self->_currentTab == TAB_MODS && self->_modsContentView) {
                    [self updateModsList];
                }
            } @catch (NSException *e) {
                NSLog(@"[BG3SE Console] Exception updating views: %@", e);
            }
        });

    } @catch (NSException *exception) {
        NSLog(@"[BG3SE Console] Exception in tabClicked: %@ - %@", exception.name, exception.reason);
    }
}

// Update mods list display - with safety checks
- (void)updateModsList {
    @try {
        // Safety: verify view exists
        if (!_modsContentView) {
            NSLog(@"[BG3SE Console] updateModsList: modsContentView is nil");
            return;
        }

        // Clear existing content
        for (NSView *subview in [_modsContentView.subviews copy]) {
            [subview removeFromSuperview];
        }

        // Placeholder mod data - in real implementation, this comes from the mod loader
        NSArray *mods = @[
            @{@"name": @"EntityTest", @"version": @"1.0", @"status": @"loaded", @"author": @"BG3SE"},
            @{@"name": @"StaticDataTest", @"version": @"1.0", @"status": @"loaded", @"author": @"BG3SE"},
            @{@"name": @"ExampleMod", @"version": @"0.5", @"status": @"error", @"author": @"Community"},
        ];

        CGFloat rowHeight = 50;
        CGFloat contentHeight = _modsContentView.bounds.size.height;
        CGFloat contentWidth = _modsContentView.bounds.size.width;
        if (contentHeight <= 0) contentHeight = 300; // Fallback
        if (contentWidth <= 0) contentWidth = 700; // Fallback
        CGFloat y = contentHeight - rowHeight;

        for (NSDictionary *mod in mods) {
            NSView *row = [[NSView alloc] initWithFrame:NSMakeRect(0, y, contentWidth, rowHeight)];
            row.wantsLayer = YES;
            row.layer.backgroundColor = [NSColor colorWithRed:0.12 green:0.12 blue:0.14 alpha:1.0].CGColor;
            row.layer.cornerRadius = 4;

            // Mod name
            NSTextField *nameLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 26, 200, 18)];
            nameLabel.stringValue = mod[@"name"];
            nameLabel.font = [NSFont systemFontOfSize:14 weight:NSFontWeightMedium];
            nameLabel.textColor = TEXT_COLOR;
            nameLabel.backgroundColor = [NSColor clearColor];
            nameLabel.bordered = NO;
            nameLabel.editable = NO;
            [row addSubview:nameLabel];

            // Version and author
            NSTextField *infoLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 8, 200, 14)];
            infoLabel.stringValue = [NSString stringWithFormat:@"v%@ by %@", mod[@"version"], mod[@"author"]];
            infoLabel.font = [NSFont systemFontOfSize:11];
            infoLabel.textColor = [NSColor colorWithRed:0.5 green:0.5 blue:0.5 alpha:1.0];
            infoLabel.backgroundColor = [NSColor clearColor];
            infoLabel.bordered = NO;
            infoLabel.editable = NO;
            [row addSubview:infoLabel];

            // Status badge
            NSString *status = mod[@"status"];
            NSColor *statusColor = [status isEqualToString:@"loaded"] ?
                [NSColor colorWithRed:0.4 green:0.9 blue:0.5 alpha:1.0] :
                [NSColor colorWithRed:1.0 green:0.4 blue:0.4 alpha:1.0];

            NSTextField *statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(contentWidth - 80, 16, 60, 18)];
            statusLabel.stringValue = [status uppercaseString];
            statusLabel.font = [NSFont systemFontOfSize:10 weight:NSFontWeightBold];
            statusLabel.textColor = statusColor;
            statusLabel.backgroundColor = [NSColor clearColor];
            statusLabel.bordered = NO;
            statusLabel.editable = NO;
            statusLabel.alignment = NSTextAlignmentRight;
            [row addSubview:statusLabel];

            [_modsContentView addSubview:row];
            y -= (rowHeight + 6);
        }

        // Resize content view to fit all mods
        CGFloat totalHeight = mods.count * (rowHeight + 6);
        if (totalHeight > _modsContentView.bounds.size.height) {
            NSRect frame = _modsContentView.frame;
            frame.size.height = totalHeight;
            _modsContentView.frame = frame;
        }
    } @catch (NSException *exception) {
        NSLog(@"[BG3SE Console] Exception in updateModsList: %@ - %@", exception.name, exception.reason);
    }
}

- (void)closeButtonClicked:(id)sender {
    overlay_hide();
}

- (BOOL)isFlipped {
    return YES;
}

- (void)appendOutput:(NSString *)text {
    if (!text) {
        text = @"<non-UTF-8 console output omitted>";
    }
    // Coalesce bursts (test runs emit hundreds of lines) into one batched
    // append + one scroll per main-queue drain. One queued block per line,
    // each forcing a whole-document relayout via scrollToEndOfDocument:,
    // crashed TextKit under load (2026-07-28, SIGBUS in appendOutput block).
    // Backstop: the console must never be able to take the game down. Any throw
    // from here reaches uncaught_exception_handler and aborts the process.
    if (!text) return;

    @synchronized (self) {
        if (!self.pendingLines) {
            self.pendingLines = [NSMutableArray array];
        }
        [self.pendingLines addObject:text];
        if (self.flushScheduled) {
            return;
        }
        self.flushScheduled = YES;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [self flushPendingOutput];
    });
}

- (void)flushPendingOutput {
    NSArray *lines;
    @synchronized (self) {
        // This file compiles WITHOUT ARC: the strong-property setter releases
        // the array when we nil it below, so the raw local must retain first
        // or it dangles (PAC crash in objc_msgSend at boot log volume,
        // flushPendingOutput+108, 2026-07-29 .ips).
        lines = [[self.pendingLines retain] autorelease];
        self.pendingLines = nil;
        self.flushScheduled = NO;
    }
    if (lines.count == 0) {
        return;
    }

    NSFont *font = [NSFont fontWithName:@"Menlo" size:FONT_SIZE];
    NSMutableAttributedString *batch = [[NSMutableAttributedString alloc] init];
    NSTextStorage *storage = [self.outputView textStorage];
    for (id line in lines) {
        if (line == [NSNull null]) {
            // Clear sentinel: wipe everything flushed so far AND everything
            // batched before it in this drain. Lines after it survive.
            [storage deleteCharactersInRange:NSMakeRange(0, storage.length)];
            batch = [[NSMutableAttributedString alloc] init];
            continue;
        }
        NSColor *textColor = colorForLogLevel([line UTF8String]);
        [batch appendAttributedString:[[NSAttributedString alloc]
            initWithString:[line stringByAppendingString:@"\n"]
            attributes:@{
                NSForegroundColorAttributeName: textColor,
                NSFontAttributeName: font
            }]];
    }

    [storage appendAttributedString:batch];

    // Bound the backlog so layout cost can't grow without limit
    if (storage.length > 500000) {
        [storage deleteCharactersInRange:NSMakeRange(0, storage.length - 400000)];
    }

    [self.outputView scrollRangeToVisible:NSMakeRange(storage.length, 0)];
}

- (void)clearOutput {
    // Linearize the wipe with appendOutput by sending it through the same
    // ordered pending stream: replacing the queue with a single clear
    // sentinel drops every pre-clear line, and any append that lands after
    // this block sits behind the sentinel, so a shared scheduled flush can
    // never erase post-clear text (or replay pre-clear text). One
    // synchronization domain, one main-queue drain — no cross-block races.
    @synchronized (self) {
        self.pendingLines = [NSMutableArray arrayWithObject:[NSNull null]];
        if (self.flushScheduled) {
            return;
        }
        self.flushScheduled = YES;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [self flushPendingOutput];
    });
}

- (void)focusInput {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.inputField becomeFirstResponder];
    });
}

// Handle Enter key to submit command
- (void)controlTextDidEndEditing:(NSNotification *)notification {
    NSTextField *textField = notification.object;
    if (textField == _inputField) {
        // Only submit when the user pressed Enter. Focus loss (e.g. clicking tabs)
        // can also end editing and must NOT execute commands.
        NSNumber *movement = notification.userInfo[@"NSTextMovement"];
        if (!movement || movement.integerValue != NSReturnTextMovement) {
            return;
        }

        NSString *command = [_inputField.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (command.length > 0) {
            // Add to history
            [_commandHistory addObject:command];
            _historyIndex = _commandHistory.count;

            // Echo command to output
            [self appendOutput:[NSString stringWithFormat:@"> %@", command]];

            // Clear input
            _inputField.stringValue = @"";

            // Call callback
            if (s_command_callback) {
                s_command_callback([command UTF8String]);
            }
        }
    }
}

// Handle up/down arrows for history and Escape to close
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)commandSelector {
    if (control == _inputField) {
        if (commandSelector == @selector(moveUp:)) {
            // Previous command
            if (_historyIndex > 0) {
                _historyIndex--;
                _inputField.stringValue = _commandHistory[_historyIndex];
            }
            return YES;
        } else if (commandSelector == @selector(moveDown:)) {
            // Next command
            if (_historyIndex < (NSInteger)_commandHistory.count - 1) {
                _historyIndex++;
                _inputField.stringValue = _commandHistory[_historyIndex];
            } else {
                _historyIndex = _commandHistory.count;
                _inputField.stringValue = @"";
            }
            return YES;
        } else if (commandSelector == @selector(cancelOperation:)) {
            // Escape key - hide overlay
            overlay_hide();
            return YES;
        }
    }
    return NO;
}

@end

// ============================================================================
// Overlay Window - Floating above game
// ============================================================================

@interface BG3SEOverlayWindow : NSWindow
@end

@implementation BG3SEOverlayWindow

- (instancetype)init {
    // Get main screen size
    NSRect screenRect = [[NSScreen mainScreen] frame];

    // Position at top-center of screen
    CGFloat x = (screenRect.size.width - OVERLAY_WIDTH) / 2;
    CGFloat y = screenRect.size.height - OVERLAY_HEIGHT - 50;

    NSRect windowRect = NSMakeRect(x, y, OVERLAY_WIDTH, OVERLAY_HEIGHT);

    self = [super initWithContentRect:windowRect
                            styleMask:NSWindowStyleMaskBorderless
                              backing:NSBackingStoreBuffered
                                defer:NO];

    if (self) {
        // Configure window
        self.level = NSScreenSaverWindowLevel;  // Very high level, above fullscreen
        self.backgroundColor = [NSColor clearColor];
        self.opaque = NO;
        self.hasShadow = YES;
        self.movableByWindowBackground = YES;
        self.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                   NSWindowCollectionBehaviorFullScreenAuxiliary;

        // Create console view
        s_console_view = [[BG3SEConsoleView alloc] initWithFrame:NSMakeRect(0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT)];
        self.contentView = s_console_view;
    }

    return self;
}

// Allow key events even when not focused
- (BOOL)canBecomeKeyWindow {
    return YES;
}

- (BOOL)canBecomeMainWindow {
    return NO;
}

@end

// ============================================================================
// Minimal Native Settings Panel
// ============================================================================

@interface BG3SERangeSlider : NSControl
@property (nonatomic) double minValue;
@property (nonatomic) double maxValue;
@property (nonatomic) double lowerValue;
@property (nonatomic) double upperValue;
- (void)setLowerValue:(double)lower upperValue:(double)upper;
@end

@implementation BG3SERangeSlider {
    BOOL _draggingLowerThumb;
}

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        _minValue = 0.0;
        _maxValue = 1.0;
        _lowerValue = 0.25;
        _upperValue = 0.75;
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

- (double)valueForX:(CGFloat)x {
    const CGFloat inset = 9.0;
    CGFloat usable = fmax(1.0, NSWidth(self.bounds) - inset * 2.0);
    double fraction = (x - inset) / usable;
    fraction = fmax(0.0, fmin(1.0, fraction));
    return self.minValue + fraction * (self.maxValue - self.minValue);
}

- (CGFloat)xForValue:(double)value {
    const CGFloat inset = 9.0;
    double span = fmax(0.0001, self.maxValue - self.minValue);
    double fraction = (value - self.minValue) / span;
    return inset + fraction * (NSWidth(self.bounds) - inset * 2.0);
}

- (void)setLowerValue:(double)lower upperValue:(double)upper {
    _lowerValue = fmax(self.minValue, fmin(lower, self.maxValue));
    _upperValue = fmax(_lowerValue, fmin(upper, self.maxValue));
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    CGFloat centerY = NSMidY(self.bounds);
    CGFloat lowerX = [self xForValue:self.lowerValue];
    CGFloat upperX = [self xForValue:self.upperValue];

    NSBezierPath *track = [NSBezierPath bezierPathWithRoundedRect:
        NSMakeRect(9.0, centerY - 2.0, NSWidth(self.bounds) - 18.0, 4.0)
        xRadius:2.0 yRadius:2.0];
    [[NSColor colorWithWhite:0.32 alpha:1.0] setFill];
    [track fill];

    NSBezierPath *selection = [NSBezierPath bezierPathWithRoundedRect:
        NSMakeRect(lowerX, centerY - 2.0, fmax(1.0, upperX - lowerX), 4.0)
        xRadius:2.0 yRadius:2.0];
    [TANIT_SECONDARY setFill];
    [selection fill];

    for (NSNumber *position in @[@(lowerX), @(upperX)]) {
        NSRect thumbRect = NSMakeRect(position.doubleValue - 7.0,
                                      centerY - 7.0, 14.0, 14.0);
        NSBezierPath *thumb = [NSBezierPath bezierPathWithOvalInRect:thumbRect];
        [TANIT_PRIMARY setFill];
        [thumb fill];
        [[NSColor colorWithWhite:0.08 alpha:0.9] setStroke];
        thumb.lineWidth = 1.0;
        [thumb stroke];
    }
}

- (void)mouseDown:(NSEvent *)event {
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    CGFloat lowerDistance = fabs(point.x - [self xForValue:self.lowerValue]);
    CGFloat upperDistance = fabs(point.x - [self xForValue:self.upperValue]);
    _draggingLowerThumb = lowerDistance <= upperDistance;

    while (YES) {
        point = [self convertPoint:event.locationInWindow fromView:nil];
        double value = [self valueForX:point.x];
        if (_draggingLowerThumb) {
            _lowerValue = fmax(self.minValue, fmin(value, self.upperValue));
        } else {
            _upperValue = fmin(self.maxValue, fmax(value, self.lowerValue));
        }
        [self setNeedsDisplay:YES];

        if (event.type == NSEventTypeLeftMouseUp) break;
        event = [self.window nextEventMatchingMask:
            NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp];
    }
    [self sendAction:self.action to:self.target];
}

@end

@interface BG3SETintedSlider : NSSlider
@end

@implementation BG3SETintedSlider

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const CGFloat inset = 9.0;
    CGFloat centerY = NSMidY(self.bounds);
    CGFloat usable = NSWidth(self.bounds) - inset * 2.0;
    double span = fmax(0.0001, self.maxValue - self.minValue);
    double fraction = (self.doubleValue - self.minValue) / span;
    fraction = fmax(0.0, fmin(1.0, fraction));
    CGFloat thumbX = inset + fraction * usable;

    NSBezierPath *track = [NSBezierPath bezierPathWithRoundedRect:
        NSMakeRect(inset, centerY - 2.0, usable, 4.0)
        xRadius:2.0 yRadius:2.0];
    [[NSColor colorWithWhite:0.32 alpha:1.0] setFill];
    [track fill];

    NSBezierPath *fill = [NSBezierPath bezierPathWithRoundedRect:
        NSMakeRect(inset, centerY - 2.0, fmax(1.0, thumbX - inset), 4.0)
        xRadius:2.0 yRadius:2.0];
    [TANIT_SECONDARY setFill];
    [fill fill];

    NSBezierPath *thumb = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(thumbX - 7.0, centerY - 7.0, 14.0, 14.0)];
    [TANIT_PRIMARY setFill];
    [thumb fill];
    [[NSColor colorWithWhite:0.08 alpha:0.9] setStroke];
    [thumb stroke];
}

@end

@interface BG3SEFramingControl : NSControl
@property (nonatomic) double horizontalValue;
@property (nonatomic) double verticalValue;
- (void)setHorizontalValue:(double)horizontal verticalValue:(double)vertical;
@end

@implementation BG3SEFramingControl

- (BOOL)isFlipped {
    return YES;
}

- (NSRect)plotRect {
    return NSInsetRect(self.bounds, 10.0, 10.0);
}

- (void)setHorizontalValue:(double)horizontal verticalValue:(double)vertical {
    _horizontalValue = fmax(-2.0, fmin(2.0, horizontal));
    _verticalValue = fmax(-1.0, fmin(3.0, vertical));
    [self setNeedsDisplay:YES];
}

- (void)setValuesForPoint:(NSPoint)point {
    NSRect plot = [self plotRect];
    double horizontal = (point.x - NSMinX(plot)) / NSWidth(plot);
    double vertical = 1.0 - ((point.y - NSMinY(plot)) / NSHeight(plot));
    horizontal = fmax(0.0, fmin(1.0, horizontal));
    vertical = fmax(0.0, fmin(1.0, vertical));
    _horizontalValue = -2.0 + horizontal * 4.0;
    _verticalValue = -1.0 + vertical * 4.0;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NSRect plot = [self plotRect];
    NSBezierPath *background = [NSBezierPath bezierPathWithRoundedRect:plot
                                                               xRadius:7.0
                                                               yRadius:7.0];
    [[NSColor colorWithWhite:0.16 alpha:1.0] setFill];
    [background fill];
    [[NSColor colorWithWhite:0.34 alpha:1.0] setStroke];
    background.lineWidth = 1.0;
    [background stroke];

    [[NSColor colorWithWhite:0.42 alpha:0.55] setStroke];
    NSBezierPath *axes = [NSBezierPath bezierPath];
    [axes moveToPoint:NSMakePoint(NSMidX(plot), NSMinY(plot) + 8.0)];
    [axes lineToPoint:NSMakePoint(NSMidX(plot), NSMaxY(plot) - 8.0)];
    [axes moveToPoint:NSMakePoint(NSMinX(plot) + 8.0, NSMidY(plot))];
    [axes lineToPoint:NSMakePoint(NSMaxX(plot) - 8.0, NSMidY(plot))];
    axes.lineWidth = 1.0;
    [axes stroke];

    double horizontalFraction = (self.horizontalValue + 2.0) / 4.0;
    double verticalFraction = (self.verticalValue + 1.0) / 4.0;
    CGFloat x = NSMinX(plot) + horizontalFraction * NSWidth(plot);
    CGFloat y = NSMaxY(plot) - verticalFraction * NSHeight(plot);
    NSBezierPath *dot = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(x - 7.0, y - 7.0, 14.0, 14.0)];
    [TANIT_PRIMARY setFill];
    [dot fill];
    [[NSColor colorWithWhite:0.08 alpha:0.9] setStroke];
    [dot stroke];
}

- (void)mouseDown:(NSEvent *)event {
    while (YES) {
        NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
        [self setValuesForPoint:point];
        if (event.type == NSEventTypeLeftMouseUp) break;
        event = [self.window nextEventMatchingMask:
            NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp];
    }
    [self sendAction:self.action to:self.target];
}

@end

@interface BG3SESettingsPanel : NSPanel
@property (nonatomic, strong) NSButton *profileCheckbox;
@property (nonatomic, strong) NSButton *capsLockMouseLookCheckbox;
@property (nonatomic, strong) NSButton *crouchCheckbox;
@property (nonatomic, strong) NSButton *hideUICheckbox;
@property (nonatomic, strong) NSButton *wheelCheckbox;
@property (nonatomic, strong) NSButton *invertCheckbox;
@property (nonatomic, strong) NSSegmentedControl *profileSelector;
@property (nonatomic, strong) NSButton *saveButton;
@property (nonatomic, strong) NSTextField *saveStatusLabel;
@property (nonatomic, strong) BG3SERangeSlider *pitchRange;
@property (nonatomic, strong) NSTextField *pitchValueLabel;
@property (nonatomic, strong) BG3SEFramingControl *framingControl;
@property (nonatomic, strong) NSTextField *horizontalValueLabel;
@property (nonatomic, strong) NSTextField *verticalValueLabel;
@property (nonatomic, strong) NSMutableDictionary<NSString *, NSSlider *> *sliders;
@property (nonatomic, strong) NSMutableDictionary<NSString *, NSTextField *> *valueLabels;
- (void)updateTitle:(NSString *)title
            message:(NSString *)message
           settings:(const OverlayCameraSettings *)settings;
- (void)selectProfile:(NSInteger)profile;
@end

@implementation BG3SESettingsPanel {
    OverlayCameraProfile _profiles[OVERLAY_CAMERA_PROFILE_COUNT];
    BOOL _profileDirty[OVERLAY_CAMERA_PROFILE_COUNT];
    BOOL _profilesInitialized;
}

- (instancetype)init {
    NSRect screenRect = [[NSScreen mainScreen] visibleFrame];
    const CGFloat width = 560.0;
    const CGFloat height = 690.0;
    NSRect frame = NSMakeRect(NSMidX(screenRect) - width / 2.0,
                              NSMidY(screenRect) - height / 2.0,
                              width, height);

    self = [super initWithContentRect:frame
                             styleMask:NSWindowStyleMaskTitled |
                                       NSWindowStyleMaskClosable
                               backing:NSBackingStoreBuffered
                                 defer:NO];
    if (self) {
        self.title = @"Native Camera Tweaks";
        self.level = NSScreenSaverWindowLevel;
        self.releasedWhenClosed = NO;
        self.hidesOnDeactivate = NO;
        self.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                  NSWindowCollectionBehaviorFullScreenAuxiliary;

        NSView *content = self.contentView;
        content.wantsLayer = YES;
        content.layer.backgroundColor = [NSColor colorWithRed:0.08
                                                        green:0.08
                                                         blue:0.10
                                                        alpha:0.98].CGColor;

        self.sliders = [NSMutableDictionary dictionary];
        self.valueLabels = [NSMutableDictionary dictionary];
        self.profileCheckbox = [NSButton checkboxWithTitle:@"Enable Player Immersive Camera"
                                                    target:self
                                                    action:@selector(profileChanged:)];
        self.profileCheckbox.frame = NSMakeRect(24, 638, width - 48, 24);
        self.profileCheckbox.font = [NSFont systemFontOfSize:13
                                                     weight:NSFontWeightSemibold];
        [content addSubview:self.profileCheckbox];

        self.capsLockMouseLookCheckbox = [NSButton checkboxWithTitle:@"Caps Lock mouse look"
                                                               target:self
                                                               action:@selector(capsLockMouseLookChanged:)];
        self.capsLockMouseLookCheckbox.frame = NSMakeRect(24, 610, width - 48, 24);
        [content addSubview:self.capsLockMouseLookCheckbox];
        NSTextField *mouseLookHelp = [self addLabel:
            @"Toggle free camera look without holding the middle mouse button."
            frame:NSMakeRect(45, 591, width - 69, 18) dim:YES];
        mouseLookHelp.font = [NSFont systemFontOfSize:11];

        [self addSectionTitle:@"PROFILES" y:554.0];
        self.profileSelector = [[NSSegmentedControl alloc]
            initWithFrame:NSMakeRect(24, 514, width - 48, 30)];
        self.profileSelector.segmentCount = 4;
        self.profileSelector.trackingMode = NSSegmentSwitchTrackingSelectOne;
        self.profileSelector.segmentStyle = NSSegmentStyleRounded;
        for (NSInteger index = 0; index < 4; index++) {
            [self.profileSelector setLabel:
                [NSString stringWithFormat:@"Profile %ld", (long)index + 1]
                                  forSegment:index];
            [self.profileSelector setWidth:(width - 48) / 4.0
                                  forSegment:index];
        }
        self.profileSelector.selectedSegment = 0;
        self.profileSelector.target = self;
        self.profileSelector.action = @selector(selectedProfileChanged:);
        [content addSubview:self.profileSelector];

        self.wheelCheckbox = [NSButton checkboxWithTitle:
            @"Include this profile in mouse-wheel switching"
                                                   target:self
                                                   action:@selector(wheelProfileChanged:)];
        self.wheelCheckbox.frame = NSMakeRect(24, 480, width - 210, 24);
        self.wheelCheckbox.state = NSControlStateValueOn;
        [content addSubview:self.wheelCheckbox];

        self.saveStatusLabel = [NSTextField labelWithString:@""];
        self.saveStatusLabel.frame = NSMakeRect(width - 252, 482, 82, 20);
        self.saveStatusLabel.textColor = DIM_TEXT_COLOR;
        self.saveStatusLabel.alignment = NSTextAlignmentRight;
        self.saveStatusLabel.font = [NSFont systemFontOfSize:11];
        [content addSubview:self.saveStatusLabel];

        self.saveButton = [NSButton buttonWithTitle:@"Save Profile 1"
                                             target:self
                                             action:@selector(saveProfile:)];
        self.saveButton.frame = NSMakeRect(width - 164, 476, 140, 30);
        self.saveButton.bezelStyle = NSBezelStyleRounded;
        [content addSubview:self.saveButton];

        [self addSectionTitle:@"CAMERA" y:442.0];
        [self addSliderWithTitle:@"Camera distance"
                              key:@"CloseZoom"
                             min:1.5 max:15.0 y:407.0];
        [self addSliderWithTitle:@"Field of view"
                              key:@"FOV"
                             min:40.0 max:90.0 y:371.0];

        [self addSectionTitle:@"FRAMING" y:334.0];
        NSTextField *framingHelp = [self addLabel:
            @"Position the character within the view."
            frame:NSMakeRect(24, 312, width - 48, 18) dim:YES];
        framingHelp.font = [NSFont systemFontOfSize:11];

        self.framingControl = [[BG3SEFramingControl alloc]
            initWithFrame:NSMakeRect(24, 205, 270, 100)];
        self.framingControl.target = self;
        self.framingControl.action = @selector(framingChanged:);
        [content addSubview:self.framingControl];

        [self addLabel:@"Horizontal"
                 frame:NSMakeRect(326, 274, 100, 20) dim:NO];
        self.horizontalValueLabel = [self addLabel:@"0.00"
            frame:NSMakeRect(430, 274, 76, 20) dim:NO];
        self.horizontalValueLabel.alignment = NSTextAlignmentRight;
        self.horizontalValueLabel.font = [NSFont monospacedDigitSystemFontOfSize:12
                                                                    weight:NSFontWeightRegular];
        [self addLabel:@"Vertical"
                 frame:NSMakeRect(326, 239, 100, 20) dim:NO];
        self.verticalValueLabel = [self addLabel:@"0.00"
            frame:NSMakeRect(430, 239, 76, 20) dim:NO];
        self.verticalValueLabel.alignment = NSTextAlignmentRight;
        self.verticalValueLabel.font = [NSFont monospacedDigitSystemFontOfSize:12
                                                                  weight:NSFontWeightRegular];

        [self addSectionTitle:@"LOOKING" y:188.0];
        NSTextField *pitchHelp = [self addLabel:
            @"Limits how far the camera can look up and down."
            frame:NSMakeRect(24, 166, width - 48, 18) dim:YES];
        pitchHelp.font = [NSFont systemFontOfSize:11];
        [self addLabel:@"Pitch range"
                 frame:NSMakeRect(24, 130, 105, 24) dim:NO];
        self.pitchRange = [[BG3SERangeSlider alloc]
            initWithFrame:NSMakeRect(130, 130, 300, 24)];
        self.pitchRange.minValue = -30.0;
        self.pitchRange.maxValue = 70.0;
        self.pitchRange.target = self;
        self.pitchRange.action = @selector(pitchRangeChanged:);
        [content addSubview:self.pitchRange];
        self.pitchValueLabel = [self addLabel:@""
            frame:NSMakeRect(432, 130, 104, 24) dim:NO];
        self.pitchValueLabel.alignment = NSTextAlignmentRight;
        self.pitchValueLabel.font = [NSFont monospacedDigitSystemFontOfSize:12
                                                               weight:NSFontWeightRegular];

        self.invertCheckbox = [NSButton checkboxWithTitle:@"Invert vertical look"
                                                   target:self
                                                   action:@selector(invertChanged:)];
        self.invertCheckbox.frame = NSMakeRect(24, 101, width - 48, 24);
        [content addSubview:self.invertCheckbox];

        [self addSectionTitle:@"IMMERSION" y:72.0];
        self.crouchCheckbox = [NSButton checkboxWithTitle:@"Adaptive crouch camera"
                                                    target:self
                                                    action:@selector(crouchChanged:)];
        self.crouchCheckbox.frame = NSMakeRect(24, 42, 245, 24);
        [content addSubview:self.crouchCheckbox];

        self.hideUICheckbox = [NSButton checkboxWithTitle:@"Hide UI in mouse look"
                                                    target:self
                                                    action:@selector(hideUIChanged:)];
        self.hideUICheckbox.frame = NSMakeRect(292, 42, 220, 24);
        self.hideUICheckbox.toolTip = @"Hide BG3's interface while Caps Lock mouse look is active, then restore its previous state.";
        [content addSubview:self.hideUICheckbox];
    }
    return self;
}

- (NSTextField *)addLabel:(NSString *)text frame:(NSRect)frame dim:(BOOL)dim {
    NSTextField *label = [NSTextField labelWithString:text];
    label.frame = frame;
    label.textColor = dim ? DIM_TEXT_COLOR : TEXT_COLOR;
    [self.contentView addSubview:label];
    return label;
}

- (void)addSectionTitle:(NSString *)title y:(CGFloat)y {
    NSTextField *label = [self addLabel:title
        frame:NSMakeRect(24, y, 112, 18) dim:YES];
    label.font = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];

    NSBox *line = [[NSBox alloc] initWithFrame:NSMakeRect(132, y + 7,
                                                          404, 1)];
    line.boxType = NSBoxSeparator;
    [self.contentView addSubview:line];
}

- (NSTextField *)addValueLabelForKey:(NSString *)key frame:(NSRect)frame {
    NSTextField *label = [self addLabel:@"" frame:frame dim:NO];
    label.textColor = TEXT_COLOR;
    label.alignment = NSTextAlignmentRight;
    label.font = [NSFont monospacedDigitSystemFontOfSize:12
                                                  weight:NSFontWeightRegular];
    self.valueLabels[key] = label;
    return label;
}

- (void)addSliderWithTitle:(NSString *)title
                       key:(NSString *)key
                       min:(double)minimum
                       max:(double)maximum
                         y:(CGFloat)y {
    [self addLabel:title frame:NSMakeRect(24, y, 130, 24) dim:NO];

    BG3SETintedSlider *slider = [[BG3SETintedSlider alloc]
        initWithFrame:NSMakeRect(155, y, 245, 24)];
    slider.minValue = minimum;
    slider.maxValue = maximum;
    slider.doubleValue = minimum;
    slider.target = self;
    slider.action = @selector(sliderChanged:);
    slider.frame = NSMakeRect(155, y, 245, 24);
    slider.identifier = key;
    slider.continuous = NO;
    [self.contentView addSubview:slider];
    self.sliders[key] = slider;

    [self addValueLabelForKey:key frame:NSMakeRect(478, y, 58, 24)];
}

- (void)setSlider:(NSString *)key value:(double)value {
    NSSlider *slider = self.sliders[key];
    if (!slider) return;
    double clamped = value;
    if (clamped < slider.minValue) clamped = slider.minValue;
    if (clamped > slider.maxValue) clamped = slider.maxValue;
    slider.doubleValue = clamped;
    [self updateValueLabel:key];
}

- (void)updateValueLabel:(NSString *)key {
    NSSlider *slider = self.sliders[key];
    NSTextField *label = self.valueLabels[key];
    if (!slider || !label) return;
    BOOL degrees = [key isEqualToString:@"FOV"] ||
                   [key isEqualToString:@"MinimumPitch"] ||
                   [key isEqualToString:@"MaximumPitch"];
    label.stringValue = degrees
        ? [NSString stringWithFormat:@"%.1f°", slider.doubleValue]
        : [NSString stringWithFormat:@"%.2f", slider.doubleValue];
}

- (void)updateTitle:(NSString *)title
            message:(NSString *)message
           settings:(const OverlayCameraSettings *)settings {
    self.title = title.length > 0 ? title : @"Native Camera Tweaks";
    (void)message;
    self.profileCheckbox.state = settings->enabled ? NSControlStateValueOn
                                                   : NSControlStateValueOff;
    self.capsLockMouseLookCheckbox.state = settings->caps_lock_mouse_look
        ? NSControlStateValueOn : NSControlStateValueOff;
    if (!_profilesInitialized) {
        for (NSInteger index = 0; index < OVERLAY_CAMERA_PROFILE_COUNT; index++) {
            _profiles[index] = settings->profiles[index];
            _profileDirty[index] = NO;
        }
        NSInteger selected = settings->selected_profile - 1;
        if (selected < 0) selected = 0;
        if (selected >= OVERLAY_CAMERA_PROFILE_COUNT) {
            selected = OVERLAY_CAMERA_PROFILE_COUNT - 1;
        }
        self.profileSelector.selectedSegment = selected;
        _profilesInitialized = YES;
    }
    [self loadSelectedProfile];
}

- (NSInteger)selectedProfileIndex {
    NSInteger selected = self.profileSelector.selectedSegment;
    if (selected < 0) return 0;
    if (selected >= OVERLAY_CAMERA_PROFILE_COUNT) {
        return OVERLAY_CAMERA_PROFILE_COUNT - 1;
    }
    return selected;
}

- (void)captureSelectedProfile {
    NSInteger index = [self selectedProfileIndex];
    OverlayCameraProfile *profile = &_profiles[index];
    profile->wheel_enabled =
        self.wheelCheckbox.state == NSControlStateValueOn;
    profile->distance = self.sliders[@"CloseZoom"].doubleValue;
    profile->fov = self.sliders[@"FOV"].doubleValue;
    profile->horizontal_offset = self.framingControl.horizontalValue;
    profile->vertical_offset = self.framingControl.verticalValue;
    profile->minimum_pitch = self.pitchRange.lowerValue;
    profile->maximum_pitch = self.pitchRange.upperValue;
    profile->invert_vertical =
        self.invertCheckbox.state == NSControlStateValueOn;
    profile->adaptive_crouch =
        self.crouchCheckbox.state == NSControlStateValueOn;
    profile->hide_game_ui =
        self.hideUICheckbox.state == NSControlStateValueOn;
}

- (void)loadSelectedProfile {
    NSInteger index = [self selectedProfileIndex];
    OverlayCameraProfile profile = _profiles[index];
    self.wheelCheckbox.state = profile.wheel_enabled
        ? NSControlStateValueOn : NSControlStateValueOff;
    [self setSlider:@"CloseZoom" value:profile.distance];
    [self setSlider:@"FOV" value:profile.fov];
    [self.framingControl setHorizontalValue:profile.horizontal_offset
                              verticalValue:profile.vertical_offset];
    [self updateFramingValueLabels];
    [self.pitchRange setLowerValue:profile.minimum_pitch
                        upperValue:profile.maximum_pitch];
    [self updatePitchValueLabel];
    self.invertCheckbox.state = profile.invert_vertical
        ? NSControlStateValueOn : NSControlStateValueOff;
    self.crouchCheckbox.state = profile.adaptive_crouch
        ? NSControlStateValueOn : NSControlStateValueOff;
    self.hideUICheckbox.state = profile.hide_game_ui
        ? NSControlStateValueOn : NSControlStateValueOff;
    self.saveButton.title = [NSString stringWithFormat:@"Save Profile %ld",
                                                       (long)index + 1];
    self.saveStatusLabel.stringValue = _profileDirty[index]
        ? @"Unsaved" : @"";
    self.saveStatusLabel.textColor = _profileDirty[index]
        ? TANIT_SECONDARY : DIM_TEXT_COLOR;
}

- (void)markProfileUnsaved {
    [self captureSelectedProfile];
    _profileDirty[[self selectedProfileIndex]] = YES;
    self.saveStatusLabel.stringValue = @"Unsaved";
    self.saveStatusLabel.textColor = TANIT_SECONDARY;
    [self sendSelectedProfilePreview];
}

- (void)sendSelectedProfilePreview {
    if (!s_settings_callback) return;
    NSInteger index = [self selectedProfileIndex];
    OverlayCameraProfile profile = _profiles[index];
    s_settings_callback("PreviewWheelEnabled", profile.wheel_enabled);
    s_settings_callback("PreviewDistance", profile.distance);
    s_settings_callback("PreviewFOV", profile.fov);
    s_settings_callback("PreviewHorizontalOffset", profile.horizontal_offset);
    s_settings_callback("PreviewVerticalOffset", profile.vertical_offset);
    s_settings_callback("PreviewMinimumPitch", profile.minimum_pitch);
    s_settings_callback("PreviewMaximumPitch", profile.maximum_pitch);
    s_settings_callback("PreviewInvertVertical", profile.invert_vertical);
    s_settings_callback("PreviewAdaptiveCrouch", profile.adaptive_crouch);
    s_settings_callback("PreviewHideGameUI", profile.hide_game_ui);
    s_settings_callback("PreviewProfile", index + 1);
}

- (void)profileChanged:(id)sender {
    (void)sender;
    if (s_settings_callback) {
        s_settings_callback("Enabled",
            self.profileCheckbox.state == NSControlStateValueOn ? 1.0 : 0.0);
    }
}

- (void)invertChanged:(id)sender {
    (void)sender;
    [self markProfileUnsaved];
}

- (void)capsLockMouseLookChanged:(id)sender {
    (void)sender;
    if (s_settings_callback) {
        s_settings_callback("CapsLockMouseLook",
            self.capsLockMouseLookCheckbox.state == NSControlStateValueOn
                ? 1.0 : 0.0);
    }
}

- (void)crouchChanged:(id)sender {
    (void)sender;
    [self markProfileUnsaved];
}

- (void)hideUIChanged:(id)sender {
    (void)sender;
    [self markProfileUnsaved];
}

- (void)selectedProfileChanged:(NSSegmentedControl *)sender {
    NSInteger profile = sender.selectedSegment + 1;
    [self loadSelectedProfile];
    if (s_settings_callback) {
        s_settings_callback("SelectedProfile", profile);
    }
    [self sendSelectedProfilePreview];
}

- (void)selectProfile:(NSInteger)profile {
    NSInteger index = profile - 1;
    if (index < 0 || index >= OVERLAY_CAMERA_PROFILE_COUNT) return;
    self.profileSelector.selectedSegment = index;
    [self loadSelectedProfile];
    [self sendSelectedProfilePreview];
}

- (void)wheelProfileChanged:(id)sender {
    (void)sender;
    [self markProfileUnsaved];
}

- (void)saveProfile:(id)sender {
    (void)sender;
    [self captureSelectedProfile];
    NSInteger index = [self selectedProfileIndex];
    OverlayCameraProfile profile = _profiles[index];
    if (s_settings_callback) {
        s_settings_callback("ProfileWheelEnabled", profile.wheel_enabled);
        s_settings_callback("ProfileDistance", profile.distance);
        s_settings_callback("ProfileFOV", profile.fov);
        s_settings_callback("ProfileHorizontalOffset", profile.horizontal_offset);
        s_settings_callback("ProfileVerticalOffset", profile.vertical_offset);
        s_settings_callback("ProfileMinimumPitch", profile.minimum_pitch);
        s_settings_callback("ProfileMaximumPitch", profile.maximum_pitch);
        s_settings_callback("ProfileInvertVertical", profile.invert_vertical);
        s_settings_callback("ProfileAdaptiveCrouch", profile.adaptive_crouch);
        s_settings_callback("ProfileHideGameUI", profile.hide_game_ui);
        s_settings_callback("SaveProfile", index + 1);
    }
    _profileDirty[index] = NO;
    self.saveStatusLabel.stringValue = @"Saved";
    self.saveStatusLabel.textColor = [NSColor systemGreenColor];
}

- (void)updatePitchValueLabel {
    self.pitchValueLabel.stringValue = [NSString stringWithFormat:@"%.1f° – %.1f°",
        self.pitchRange.lowerValue, self.pitchRange.upperValue];
}

- (void)updateFramingValueLabels {
    self.horizontalValueLabel.stringValue = [NSString stringWithFormat:@"%.2f",
        self.framingControl.horizontalValue];
    self.verticalValueLabel.stringValue = [NSString stringWithFormat:@"%.2f",
        self.framingControl.verticalValue];
}

- (void)framingChanged:(BG3SEFramingControl *)control {
    (void)control;
    [self updateFramingValueLabels];
    [self markProfileUnsaved];
}

- (void)pitchRangeChanged:(BG3SERangeSlider *)slider {
    (void)slider;
    [self updatePitchValueLabel];
    [self markProfileUnsaved];
}

- (void)sliderChanged:(NSSlider *)slider {
    NSString *key = slider.identifier;
    [self updateValueLabel:key];
    [self markProfileUnsaved];
}

- (void)close {
    // Keep the panel reusable when the title-bar close button is clicked.
    [self orderOut:nil];
}

- (BOOL)canBecomeKeyWindow {
    return YES;
}

@end


// ============================================================================
// Public API Implementation
// ============================================================================

void overlay_init(void) {
    if (s_initialized) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            s_overlay_window = [[BG3SEOverlayWindow alloc] init];

            // Start hidden
            [s_overlay_window orderOut:nil];

            LOG_CONSOLE_INFO("Console overlay initialized");
            s_initialized = true;
        }
    });
}

void overlay_shutdown(void) {
    if (!s_initialized) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            [s_overlay_window close];
            s_overlay_window = nil;
            s_console_view = nil;
            s_initialized = false;

            LOG_CONSOLE_INFO("Console overlay shutdown");
        }
    });
}

void overlay_toggle(void) {
    if (!s_initialized || !s_overlay_window) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            if ([s_overlay_window isVisible]) {
                [s_overlay_window orderOut:nil];
                LOG_CONSOLE_INFO("Hidden");
            } else {
                [s_overlay_window makeKeyAndOrderFront:nil];
                [s_console_view focusInput];
                LOG_CONSOLE_INFO("Shown");
            }
        }
    });
}

void overlay_show(void) {
    if (!s_initialized || !s_overlay_window) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            [s_overlay_window makeKeyAndOrderFront:nil];
            [s_console_view focusInput];
        }
    });
}

void overlay_hide(void) {
    if (!s_initialized || !s_overlay_window) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            [s_overlay_window orderOut:nil];
        }
    });
}

bool overlay_is_visible(void) {
    if (!s_initialized || !s_overlay_window) return false;

    __block bool visible = false;
    dispatch_sync(dispatch_get_main_queue(), ^{
        visible = [s_overlay_window isVisible];
    });
    return visible;
}

void overlay_append_output(const char *text) {
    if (!s_initialized || !s_console_view || !text) return;

    // stringWithUTF8String: returns nil for any byte sequence that is not valid
    // UTF-8, and console output routinely carries bytes straight out of game
    // memory (Ext.Debug.ReadFixedString, hex dumps, mod prints). Passing that
    // nil to appendOutput: reaches -[NSMutableArray addObject:], which throws
    // NSInvalidArgumentException, and nothing catches it: the process aborts.
    // Fall back to a lossy decode so bad bytes cost a garbled line, not the game.
    NSString *nsText = [NSString stringWithUTF8String:text];
    if (!nsText) {
        nsText = [[NSString alloc] initWithBytes:text
                                          length:strlen(text)
                                        encoding:NSISOLatin1StringEncoding];
    }
    if (!nsText) return;

    [s_console_view appendOutput:nsText];
}

void overlay_clear_output(void) {
    if (!s_initialized || !s_console_view) return;

    [s_console_view clearOutput];
}

void overlay_set_command_callback(overlay_command_callback callback) {
    s_command_callback = callback;
}

void overlay_focus_input(void) {
    if (!s_initialized || !s_console_view) return;

    [s_console_view focusInput];
}

void overlay_settings_toggle(const char *title, const char *message,
                             const OverlayCameraSettings *settings,
                             overlay_settings_callback callback) {
    NSString *panelTitle = title
        ? [NSString stringWithUTF8String:title] : @"Native Camera Tweaks";
    NSString *panelMessage = message
        ? [NSString stringWithUTF8String:message] : @"Minimal UI prototype";
    OverlayCameraSettings panelSettings = settings
        ? *settings : (OverlayCameraSettings){0};

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            s_settings_callback = callback;
            if (!s_settings_panel) {
                s_settings_panel = [[BG3SESettingsPanel alloc] init];
            }
            [s_settings_panel updateTitle:panelTitle
                                  message:panelMessage
                                 settings:&panelSettings];
            if (s_settings_panel.isVisible) {
                [s_settings_panel orderOut:nil];
                LOG_CONSOLE_INFO("Native settings panel hidden");
            } else {
                [s_settings_panel makeKeyAndOrderFront:nil];
                LOG_CONSOLE_INFO("Native settings panel shown");
            }
        }
    });
}

void overlay_settings_select_profile(int profile) {
    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            if (s_settings_panel) {
                [s_settings_panel selectProfile:profile];
            }
        }
    });
}
