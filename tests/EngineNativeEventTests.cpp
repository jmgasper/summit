#include "config.h"
#include "EditingCommandsHaiku.h"
#include "NativeWebKeyboardEvent.h"
#include "NativeWebMouseEvent.h"
#include "NativeWebWheelEvent.h"
#include "WebEventConversion.h"
#include <Application.h>
#include <InterfaceDefs.h>
#include <WebCore/Scrollbar.h>
#include <WebCore/WindowsKeyboardCodes.h>
#include <wtf/MainThread.h>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace WebKit;
static int checks = 0, failures = 0;
static void Check(bool passed, const char* label)
{
    ++checks;
    failures += !passed;
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}

static BMessage Key(uint32 what, const char* bytes, int32 key, int32 modifiers = 0)
{
    BMessage message(what);
    message.AddString("bytes", bytes);
    message.AddInt32("key", key);
    message.AddInt32("modifiers", modifiers);
    message.AddInt64("when", 12345678);
    return message;
}

static bool Command(char byte, int32 modifiers, const char* expected, bool characterEvent = false)
{
    char bytes[] = { byte, 0 };
    auto event = NativeWebKeyboardEvent::create(Key(B_KEY_DOWN, bytes, 0, modifiers));
    if (!event)
        return false;
    auto* command = editingCommandHaiku(platform(*event), characterEvent);
    return expected ? command && !std::strcmp(command, expected) : !command;
}

int main()
{
    BApplication app("application/x-vnd.Kunanyi-Summit-EventTests");
    WTF::initializeMainThread();
    auto keyMessage = Key(B_KEY_DOWN, "p", 0x30, B_COMMAND_KEY | B_SHIFT_KEY | B_CAPS_LOCK);
    auto key = NativeWebKeyboardEvent::create(keyMessage);
    Check(key && key->type() == WebEventType::KeyDown && key->text() == "p"_s
        && key->key() == "p"_s && key->code() == "KeyP"_s && key->windowsVirtualKeyCode() == VK_P,
        "native key message preserves text and corrected physical key code");
    if (!key) return 1;
    Check(key->controlKey() && key->shiftKey() && key->capsLockKey() && !key->altKey(),
        "native Command shortcuts and lock modifiers reach the web event");
    Check(key->timestamp().secondsSinceEpoch().value() == 12.345678,
        "native event timestamps retain monotonic microsecond timing");
    keyMessage.ReplaceString("bytes", "changed");
    Check(!std::strcmp(key->nativeEvent()->FindString("bytes"), "p") && key->text() == "p"_s,
        "queued keyboard event owns its original native message");
    auto convertedKey = platform(*key);
    Check(convertedKey.type() == WebCore::PlatformEvent::Type::KeyDown && convertedKey.text() == "p"_s
        && convertedKey.controlKey() && convertedKey.shiftKey() && convertedKey.code() == "KeyP"_s,
        "native keyboard data survives the WebKit-to-WebCore conversion");

    auto unicodeMessage = Key(B_KEY_DOWN, "雪", 0x3c);
    unicodeMessage.AddInt32("be:key_repeat", 2);
    auto unicode = NativeWebKeyboardEvent::create(unicodeMessage);
    Check(unicode && unicode->text() == String::fromUTF8("雪") && unicode->key() == String::fromUTF8("雪") && unicode->isAutoRepeat(),
        "Unicode text and auto-repeat survive native event translation");
    auto up = NativeWebKeyboardEvent::create(Key(B_KEY_UP, "p", 0x30));
    Check(up && up->type() == WebEventType::KeyUp && !up->isAutoRepeat(), "key release has its own event type");
    auto control = NativeWebKeyboardEvent::create(Key(B_UNMAPPED_KEY_DOWN, "", 0x5c, B_CONTROL_KEY));
    Check(control && control->key() == "Alt"_s && control->altKey() && control->text().isEmpty(),
        "unmapped native modifier keys have names and no inserted text");
    auto meta = NativeWebKeyboardEvent::create(Key(B_UNMAPPED_KEY_DOWN, "", 0x66, B_OPTION_KEY));
    Check(meta && meta->key() == "Meta"_s && meta->code() == "MetaLeft"_s && meta->metaKey(),
        "native option key reports the standard Meta key code");
    auto enter = NativeWebKeyboardEvent::create(Key(B_KEY_DOWN, "\n", 0x47));
    Check(enter && enter->key() == "Enter"_s && enter->code() == "Enter"_s, "main return key uses the standard Enter code");
    auto keypad = NativeWebKeyboardEvent::create(Key(B_KEY_DOWN, "2", 0x59));
    Check(keypad && keypad->code() == "Numpad2"_s && keypad->isKeypad(), "numeric keypad keys retain their physical location");
    Check(!NativeWebKeyboardEvent::create(BMessage(B_MOUSE_DOWN)), "keyboard adapter rejects unrelated native messages");
    Check(!NativeWebKeyboardEvent::create(Key(B_KEY_DOWN, "\xff", 0x30)), "keyboard adapter rejects invalid UTF-8 input");
    auto escape = NativeWebKeyboardEvent::create(Key(B_KEY_DOWN, "\x1b", 1));
    Check(key->isActivationTriggeringEvent() && escape && !escape->isActivationTriggeringEvent(),
        "user-activation classification distinguishes typing from Escape");

    Check(Command(B_LEFT_ARROW, 0, "MoveLeft")
        && Command(B_RIGHT_ARROW, B_SHIFT_KEY, "MoveRightAndModifySelection"),
        "native arrow keys move the caret and Shift extends selection");
    Check(Command(B_LEFT_ARROW, B_COMMAND_KEY, "MoveWordLeft")
        && Command(B_RIGHT_ARROW, B_COMMAND_KEY | B_SHIFT_KEY, "MoveWordRightAndModifySelection"),
        "native Command changes horizontal editing to word granularity");
    Check(Command(B_UP_ARROW, B_COMMAND_KEY, "MoveToBeginningOfParagraph")
        && Command(B_DOWN_ARROW, B_COMMAND_KEY | B_SHIFT_KEY, "MoveParagraphForwardAndModifySelection"),
        "native Command uses paragraph granularity for vertical editing");
    Check(Command(B_HOME, B_COMMAND_KEY, "MoveToBeginningOfDocument")
        && Command(B_END, B_COMMAND_KEY | B_SHIFT_KEY, "MoveToEndOfDocumentAndModifySelection"),
        "native document boundary shortcuts preserve selection modifiers");
    Check(Command(B_BACKSPACE, B_COMMAND_KEY, "DeleteWordBackward")
        && Command(B_DELETE, 0, "DeleteForward"),
        "native deletion shortcuts distinguish word and character granularity");
    Check(Command('c', B_COMMAND_KEY, "Copy") && Command('v', B_COMMAND_KEY, "Paste")
        && Command('x', B_COMMAND_KEY, "Cut") && Command('a', B_COMMAND_KEY, "SelectAll"),
        "native Command clipboard and select-all shortcuts reach WebCore editing");
    Check(Command('z', B_COMMAND_KEY, "Undo") && Command('z', B_COMMAND_KEY | B_SHIFT_KEY, "Redo")
        && Command('y', B_COMMAND_KEY, "Redo"),
        "native undo and redo shortcuts retain their modifier distinctions");
    Check(Command(B_TAB, 0, "InsertTab", true) && Command(B_TAB, B_SHIFT_KEY, "InsertBacktab", true)
        && Command(B_RETURN, 0, "InsertNewline", true) && Command(B_RETURN, B_SHIFT_KEY, "InsertLineBreak", true),
        "character events distinguish tab navigation and newline insertion");
    Check(Command('c', B_CONTROL_KEY, nullptr) && Command('c', B_OPTION_KEY, nullptr)
        && Command('c', 0, nullptr) && Command('c', B_COMMAND_KEY, nullptr, true),
        "typing and native alternative modifiers do not execute Command shortcuts");
    Check(Command(B_TAB, B_COMMAND_KEY, nullptr) && Command(B_RETURN, B_COMMAND_KEY, nullptr)
        && Command(B_ESCAPE, 0, "Cancel") && Command(B_ESCAPE, B_SHIFT_KEY, nullptr),
        "reserved modified keys remain available to the browser and page");

    BMessage mouseMessage(B_MOUSE_DOWN);
    mouseMessage.AddInt32("buttons", B_PRIMARY_MOUSE_BUTTON | B_SECONDARY_MOUSE_BUTTON);
    mouseMessage.AddInt32("clicks", 2);
    mouseMessage.AddInt32("modifiers", B_COMMAND_KEY);
    auto mouse = NativeWebMouseEvent::create(mouseMessage, { 12.5f, 25.25f }, { 112.5f, 225.25f }, B_PRIMARY_MOUSE_BUTTON);
    Check(mouse && mouse->button() == WebMouseEventButton::Right && mouse->buttons() == 3 && mouse->clickCount() == 2,
        "multi-button press identifies the newly pressed button");
    if (!mouse) return 1;
    Check(mouse->position() == WebCore::DoublePoint(12.5, 25.25) && mouse->globalPosition() == WebCore::DoublePoint(112.5, 225.25),
        "mouse coordinates preserve fractional view and screen positions");
    auto convertedMouse = platform(*mouse);
    Check(convertedMouse.type() == WebCore::PlatformEvent::Type::MousePressed && convertedMouse.controlKey()
        && convertedMouse.button() == WebCore::MouseButton::Right,
        "native mouse state reaches WebCore through the production converter");
    mouseMessage.ReplaceInt32("buttons", 0);
    Check(mouse->nativeEvent()->FindInt32("buttons") == 3, "queued mouse event owns its native message");
    BMessage mouseUp(B_MOUSE_UP);
    mouseUp.AddInt32("buttons", B_PRIMARY_MOUSE_BUTTON);
    auto release = NativeWebMouseEvent::create(mouseUp, { 0, 0 }, { 0, 0 }, B_PRIMARY_MOUSE_BUTTON | B_SECONDARY_MOUSE_BUTTON);
    Check(release && release->type() == WebEventType::MouseUp && release->button() == WebMouseEventButton::Right && release->buttons() == 1,
        "button release preserves the other held buttons");
    BMessage move(B_MOUSE_MOVED);
    move.AddInt32("buttons", B_PRIMARY_MOUSE_BUTTON);
    auto drag = NativeWebMouseEvent::create(move, { 3, 4 }, { 30, 40 }, B_PRIMARY_MOUSE_BUTTON);
    Check(drag && drag->type() == WebEventType::MouseMove && drag->button() == WebMouseEventButton::Left && drag->buttons() == 1,
        "mouse movement retains held-button state for dragging");
    Check(!NativeWebMouseEvent::create(move, { std::numeric_limits<float>::quiet_NaN(), 0 }, { 0, 0 }, 0)
        && !NativeWebMouseEvent::create(move, { 0, 0 }, { std::numeric_limits<float>::infinity(), 0 }, 0),
        "mouse adapter rejects invalid coordinates");

    BMessage wheelMessage(B_MOUSE_WHEEL_CHANGED);
    wheelMessage.AddFloat("be:wheel_delta_x", 0.25f);
    wheelMessage.AddFloat("be:wheel_delta_y", -2);
    wheelMessage.AddInt32("modifiers", B_SHIFT_KEY);
    auto wheel = NativeWebWheelEvent::create(wheelMessage, { 12, 25 }, { 112, 225 });
    Check(wheel && wheel->wheelTicks() == WebCore::FloatSize(-0.25f, 2) && wheel->granularity() == WebWheelEventGranularity::ScrollByPixelWheelEvent,
        "wheel translation preserves fractional ticks and native scrolling direction");
    if (!wheel) return 1;
    Check(wheel->delta() == WebCore::FloatSize(-0.25f * WebCore::Scrollbar::pixelsPerLineStep(), 2 * WebCore::Scrollbar::pixelsPerLineStep()) && wheel->shiftKey(),
        "wheel ticks become pixel deltas with the native line step");
    auto convertedWheel = platform(*wheel);
    Check(convertedWheel.deltaX() == wheel->delta().width() && convertedWheel.deltaY() == wheel->delta().height()
        && convertedWheel.position() == WebCore::IntPoint(12, 25),
        "native wheel data survives the WebKit-to-WebCore conversion");
    wheelMessage.ReplaceFloat("be:wheel_delta_y", 99);
    Check(wheel->nativeEvent()->FindFloat("be:wheel_delta_y") == -2, "queued wheel event owns its native message");
    wheelMessage.ReplaceFloat("be:wheel_delta_y", std::numeric_limits<float>::infinity());
    Check(!NativeWebWheelEvent::create(wheelMessage, { 0, 0 }, { 0, 0 }), "wheel adapter rejects non-finite deltas");
    Check(!NativeWebWheelEvent::create(BMessage(B_KEY_DOWN), { 0, 0 }, { 0, 0 }), "wheel adapter rejects unrelated native messages");
    Check(wheel->timestamp() > MonotonicTime(), "events without native timestamps receive a current monotonic timestamp");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
