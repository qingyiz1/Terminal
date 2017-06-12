#include "precomp.h"

#include "windowio.hpp"

#include "ConsoleControl.hpp"
#include "find.h"
#include "clipboard.hpp"
#include "consoleKeyInfo.hpp"
#include "window.hpp"

#include "..\..\host\init.hpp"
#include "..\..\host\input.h"
#include "..\..\host\handle.h"
#include "..\..\host\scrolling.hpp"
#include "..\..\host\output.h"

#include "..\inc\ServiceLocator.hpp"

#pragma hdrstop

using namespace Microsoft::Console::Interactivity::Win32;

// ----------------------------
// Helpers
// ----------------------------

ULONG ConvertMouseButtonState(_In_ ULONG Flag, _In_ ULONG State)
{
    if (State & MK_LBUTTON)
    {
        Flag |= FROM_LEFT_1ST_BUTTON_PRESSED;
    }
    if (State & MK_MBUTTON)
    {
        Flag |= FROM_LEFT_2ND_BUTTON_PRESSED;
    }
    if (State & MK_RBUTTON)
    {
        Flag |= RIGHTMOST_BUTTON_PRESSED;
    }

    return Flag;
}

/*
* This routine tells win32k what process we want to use to masquerade as the
* owner of conhost's window. If ProcessData is nullptr that means the root process
* has exited so we need to find any old process to be the owner. If this console
* has no processes attached to it -- it's only being kept alive by references
* via IO handles -- then we'll just set the owner to conhost.exe itself.
*/
VOID SetConsoleWindowOwner(_In_ const HWND hwnd, _Inout_opt_ ConsoleProcessHandle* pProcessData)
{
    ASSERT(ServiceLocator::LocateGlobals()->getConsoleInformation()->IsConsoleLocked());

    DWORD dwProcessId;
    DWORD dwThreadId;
    if (nullptr != pProcessData)
    {
        dwProcessId = pProcessData->dwProcessId;
        dwThreadId = pProcessData->dwThreadId;
    }
    else
    {
        // Find a process to own the console window. If there are none then let's use conhost's.
        pProcessData = ServiceLocator::LocateGlobals()->getConsoleInformation()->ProcessHandleList.GetFirstProcess();
        if (pProcessData != nullptr)
        {
            dwProcessId = pProcessData->dwProcessId;
            dwThreadId = pProcessData->dwThreadId;
            pProcessData->fRootProcess = true;
        }
        else
        {
            dwProcessId = GetCurrentProcessId();
            dwThreadId = GetCurrentThreadId();
        }
    }

    CONSOLEWINDOWOWNER ConsoleOwner;
    ConsoleOwner.hwnd = hwnd;
    ConsoleOwner.ProcessId = dwProcessId;
    ConsoleOwner.ThreadId = dwThreadId;

    // Comment out this line to enable UIA tree to be visible until UIAutomationCore.dll can support our scenario.
    ServiceLocator::LocateConsoleControl < Microsoft::Console::Interactivity::Win32::ConsoleControl > ()
        ->Control(ConsoleControl::ControlType::ConsoleSetWindowOwner,
                  &ConsoleOwner,
                  sizeof(ConsoleOwner));
}

// ----------------------------
// Window Message Handlers
// (called by windowproc)
// ----------------------------

// Routine Description:
// - Handler for detecting whether a key-press event can be appropriately converted into a terminal sequence.
//   Will only trigger when virtual terminal input mode is set via STDIN handle
// Arguments:
// - pInputRecord - Input record event from the general input event handler
// Return Value:
// - True if the modes were appropriate for converting to a terminal sequence AND there was a matching terminal sequence for this key. False otherwise.
bool HandleTerminalMouseEvent(_In_ const COORD cMousePosition, _In_ const unsigned int uiButton, _In_ const short sModifierKeystate, _In_ const short sWheelDelta)
{
    // If the modes don't align, this is unhandled by default.
    bool fWasHandled = false;

    // Virtual terminal input mode
    if (IsInVirtualTerminalInputMode())
    {
        fWasHandled = ServiceLocator::LocateGlobals()->getConsoleInformation()->terminalMouseInput.HandleMouse(cMousePosition, uiButton, sModifierKeystate, sWheelDelta);
    }

    return fWasHandled;
}

void HandleKeyEvent(_In_ const HWND hWnd, _In_ const UINT Message, _In_ const WPARAM wParam, _In_ const LPARAM lParam, _Inout_opt_ PBOOL pfUnlockConsole)
{
    BOOL bGenerateBreak = FALSE;

    // BOGUS for WM_CHAR/WM_DEADCHAR, in which LOWORD(lParam) is a character
    WORD VirtualKeyCode = LOWORD(wParam);
    const ULONG ControlKeyState = GetControlKeyState(lParam);
    const BOOL bKeyDown = IsFlagClear(lParam, KEY_TRANSITION_UP);

    if (bKeyDown)
    {
        // Log a telemetry flag saying the user interacted with the Console
        // Only log when the key is a down press.  Otherwise we're getting many calls with
        // Message = WM_CHAR, VirtualKeyCode = VK_TAB, with bKeyDown = false
        // when nothing is happening, or the user has merely clicked on the title bar, and
        // this can incorrectly mark the session as being interactive.
        Telemetry::Instance().SetUserInteractive();
    }

    // Make sure we retrieve the key info first, or we could chew up unneeded space in the key info table if we bail out early.
    INPUT_RECORD InputEvent;
    InputEvent.Event.KeyEvent.wVirtualKeyCode = VirtualKeyCode;
    InputEvent.Event.KeyEvent.wVirtualScanCode = (BYTE)(HIWORD(lParam));
    if (Message == WM_CHAR || Message == WM_SYSCHAR || Message == WM_DEADCHAR || Message == WM_SYSDEADCHAR)
    {
        // --- START LOAD BEARING CODE ---
        // NOTE: We MUST match up the original data from the WM_KEYDOWN stroke (handled at some inexact moment in the
        //       past by TranslateMessageEx) with the WM_CHAR we are processing now to ensure we have the correct
        //       wVirtualScanCode to associate with the message and pass down into the console input queue for further
        //       processing.
        //       This is required because we cannot accurately re-synthesize (using MapVirtualKey/Ex)
        //       the original scan code just based on the information we have now and the scan code might be 
        //       required by the underlying client application, processed input handler (inside the console),
        //       or other input channels to help portray certain key sequences.
        //       Most notably this affects Ctrl-C, Ctrl-Break, and Pause/Break among others.
        //
        RetrieveKeyInfo(hWnd,
                        &InputEvent.Event.KeyEvent.wVirtualKeyCode,
                        &InputEvent.Event.KeyEvent.wVirtualScanCode,
                        !ServiceLocator::LocateGlobals()->getConsoleInformation()->pInputBuffer->fInComposition);

        VirtualKeyCode = InputEvent.Event.KeyEvent.wVirtualKeyCode;
        // --- END LOAD BEARING CODE ---
    }

    InputEvent.EventType = KEY_EVENT;
    InputEvent.Event.KeyEvent.bKeyDown = bKeyDown;
    InputEvent.Event.KeyEvent.wRepeatCount = LOWORD(lParam);

    if (Message == WM_CHAR || Message == WM_SYSCHAR || Message == WM_DEADCHAR || Message == WM_SYSDEADCHAR)
    {
        // If this is a fake character, zero the scancode.
        if (lParam & 0x02000000)
        {
            InputEvent.Event.KeyEvent.wVirtualScanCode = 0;
        }
        InputEvent.Event.KeyEvent.dwControlKeyState = GetControlKeyState(lParam);
        if (Message == WM_CHAR || Message == WM_SYSCHAR)
        {
            InputEvent.Event.KeyEvent.uChar.UnicodeChar = (WCHAR)wParam;
        }
        else
        {
            InputEvent.Event.KeyEvent.uChar.UnicodeChar = (WCHAR)0;
        }
    }
    else
    {
        // if alt-gr, ignore
        if (lParam & 0x02000000)
        {
            return;
        }
        InputEvent.Event.KeyEvent.dwControlKeyState = ControlKeyState;
        InputEvent.Event.KeyEvent.uChar.UnicodeChar = 0;
    }

    const INPUT_KEY_INFO inputKeyInfo(VirtualKeyCode, ControlKeyState);

    // Capture telemetry on Ctrl+Shift+ C or V commands
    if (IsInProcessedInputMode())
    {
        // Capture telemetry data when a user presses ctrl+shift+c or v in processed mode
        if (inputKeyInfo.IsShiftAndCtrlOnly()) 
        {
            if (VirtualKeyCode == 'V')
            {
                Telemetry::Instance().LogCtrlShiftVProcUsed();
            } 
            else if (VirtualKeyCode == 'C')
            {
                Telemetry::Instance().LogCtrlShiftCProcUsed();
            }
        }
    }
    else
    {
        // Capture telemetry data when a user presses ctrl+shift+c or v in raw mode
        if (inputKeyInfo.IsShiftAndCtrlOnly())
        {
            if (VirtualKeyCode == 'V')
            {
                Telemetry::Instance().LogCtrlShiftVRawUsed();
            }
            else if (VirtualKeyCode == 'C')
            {
                Telemetry::Instance().LogCtrlShiftCRawUsed();
            }
        }
    }

    // If this is a key up message, should we ignore it? We do this so that if a process reads a line from the input
    // buffer, the key up event won't get put in the buffer after the read completes.
    if (ServiceLocator::LocateGlobals()->getConsoleInformation()->Flags & CONSOLE_IGNORE_NEXT_KEYUP)
    {
        ServiceLocator::LocateGlobals()->getConsoleInformation()->Flags &= ~CONSOLE_IGNORE_NEXT_KEYUP;
        if (!bKeyDown)
        {
            return;
        }
    }

    Selection* pSelection = &Selection::Instance();

    if (!IsInVirtualTerminalInputMode())
    {
        // First attempt to process simple key chords (Ctrl+Key)
        if (inputKeyInfo.IsCtrlOnly() && ShouldTakeOverKeyboardShortcuts() && bKeyDown)
        {
            switch (VirtualKeyCode)
            {
            case 'A':
                // Set Text Selection using keyboard to true for telemetry
                Telemetry::Instance().SetKeyboardTextSelectionUsed();
                // the user is asking to select all
                pSelection->SelectAll();
                return;
            case 'F':
                // the user is asking to go to the find window
                DoFind();
                *pfUnlockConsole = FALSE;
                return;
            case 'M':
                // the user is asking for mark mode
                Selection::Instance().InitializeMarkSelection();
                return;
            case 'V':
                // the user is attempting to paste from the clipboard
                Telemetry::Instance().SetKeyboardTextEditingUsed();
                Clipboard::Instance().Paste();
                return;
            case VK_HOME:
            case VK_END:
            case VK_UP:
            case VK_DOWN:
                // if the user is asking for keyboard scroll, give it to them
                if (Scrolling::s_HandleKeyScrollingEvent(&inputKeyInfo))
                {
                    return;
                }
                break;
            case VK_PRIOR:
            case VK_NEXT:
                Telemetry::Instance().SetCtrlPgUpPgDnUsed();
                break;
            }
        }

        // Handle F11 fullscreen toggle
        if (VirtualKeyCode == VK_F11 &&
            bKeyDown &&
            inputKeyInfo.HasNoModifiers() &&
            ShouldTakeOverKeyboardShortcuts())
        {
            ServiceLocator::LocateConsoleWindow<Window>()->ToggleFullscreen();
            return;
        }

        // handle shift-ins paste
        if (inputKeyInfo.IsShiftOnly() && ShouldTakeOverKeyboardShortcuts())
        {
            if (!bKeyDown)
            {
                return;
            }
            else if (VirtualKeyCode == VK_INSERT && !(pSelection->IsInSelectingState() && pSelection->IsKeyboardMarkSelection()))
            {
                Clipboard::Instance().Paste();
                return;
            }
        }

        // handle ctrl+shift+plus/minus for transparency adjustment
        if (inputKeyInfo.IsShiftAndCtrlOnly() && ShouldTakeOverKeyboardShortcuts())
        {
            if (!bKeyDown)
            {
                return;
            }
            else
            {
                //This is the only place where the window opacity is changed NOT due to the props sheet.
                short opacityDelta = 0;
                if (VirtualKeyCode == VK_OEM_PLUS || VirtualKeyCode == VK_ADD)
                {
                    opacityDelta = OPACITY_DELTA_INTERVAL;
                }
                else if (VirtualKeyCode == VK_OEM_MINUS || VirtualKeyCode == VK_SUBTRACT)
                {
                    opacityDelta = -OPACITY_DELTA_INTERVAL;
                }
                if (opacityDelta != 0)
                {
                    ServiceLocator::LocateConsoleWindow<Window>()->ChangeWindowOpacity(opacityDelta);
                    ServiceLocator::LocateConsoleWindow()->SetWindowHasMoved(true);

                    return;
                }

            }
        }
    }

    // Then attempt to process more complicated selection/scrolling commands that require state.
    // These selection and scrolling functions must go after the simple key-chord combinations
    // as they have the potential to modify state in a way those functions do not expect.
    if (ServiceLocator::LocateGlobals()->getConsoleInformation()->Flags & CONSOLE_SELECTING)
    {
        if (!bKeyDown)
        {
            return;
        }

        Selection::KeySelectionEventResult handlingResult = pSelection->HandleKeySelectionEvent(&inputKeyInfo);
        if (handlingResult == Selection::KeySelectionEventResult::CopyToClipboard)
        {
            Clipboard::Instance().Copy();
            return;
        }
        else if (handlingResult == Selection::KeySelectionEventResult::EventHandled)
        {
            return;
        }
    }
    if (Scrolling::s_IsInScrollMode())
    {
        if (!bKeyDown || Scrolling::s_HandleKeyScrollingEvent(&inputKeyInfo))
        {
            return;
        }
    }
    if (pSelection->s_IsValidKeyboardLineSelection(&inputKeyInfo) && IsInProcessedInputMode() && ServiceLocator::LocateGlobals()->getConsoleInformation()->GetExtendedEditKey())
    {
        if (!bKeyDown || pSelection->HandleKeyboardLineSelectionEvent(&inputKeyInfo))
        {
            return;
        }
    }

    // if the user is inputting chars at an inappropriate time, beep.
    if ((ServiceLocator::LocateGlobals()->getConsoleInformation()->Flags & (CONSOLE_SELECTING | CONSOLE_SCROLLING | CONSOLE_SCROLLBAR_TRACKING)) &&
        bKeyDown &&
        !IsSystemKey(VirtualKeyCode))
    {
        ServiceLocator::LocateConsoleWindow()->SendNotifyBeep();
        return;
    }

    if (ServiceLocator::LocateGlobals()->getConsoleInformation()->pInputBuffer->fInComposition)
    {
        return;
    }

    // ignore key strokes that will generate CHAR messages. this is only necessary while a dialog box is up.
    if (ServiceLocator::LocateGlobals()->uiDialogBoxCount != 0)
    {
        if (Message != WM_CHAR && Message != WM_SYSCHAR && Message != WM_DEADCHAR && Message != WM_SYSDEADCHAR)
        {
            WCHAR awch[MAX_CHARS_FROM_1_KEYSTROKE];
            BYTE KeyState[256];
            if (GetKeyboardState(KeyState))
            {
                int cwch = ToUnicodeEx((UINT)wParam, HIWORD(lParam), KeyState, awch, ARRAYSIZE(awch), TM_POSTCHARBREAKS, nullptr);
                if (cwch != 0)
                {
                    return;
                }
            }
            else
            {
                return;
            }
        }
        else
        {
            // remember to generate break
            if (Message == WM_CHAR)
            {
                bGenerateBreak = TRUE;
            }
        }
    }

    // N.B.: This call passes InputEvent by value.
    HandleGenericKeyEvent(InputEvent, bGenerateBreak);
}

// Routine Description:
// - Returns TRUE if DefWindowProc should be called.
BOOL HandleSysKeyEvent(_In_ const HWND hWnd, _In_ const UINT Message, _In_ const WPARAM wParam, _In_ const LPARAM lParam, _Inout_opt_ PBOOL pfUnlockConsole)
{
    WORD VirtualKeyCode;

    if (Message == WM_SYSCHAR || Message == WM_SYSDEADCHAR)
    {
        VirtualKeyCode = (WORD)MapVirtualKeyW(LOBYTE(HIWORD(lParam)), MAPVK_VSC_TO_VK_EX);
    }
    else
    {
        VirtualKeyCode = LOWORD(wParam);
    }

    // Log a telemetry flag saying the user interacted with the Console
    Telemetry::Instance().SetUserInteractive();

    // check for ctrl-esc
    BOOL const bCtrlDown = GetKeyState(VK_CONTROL) & KEY_PRESSED;

    if (VirtualKeyCode == VK_ESCAPE &&
        bCtrlDown && !(GetKeyState(VK_MENU) & KEY_PRESSED) && !(GetKeyState(VK_SHIFT) & KEY_PRESSED))
    {
        return TRUE;    // call DefWindowProc
    }

    // check for alt-f4
    if (VirtualKeyCode == VK_F4 && (GetKeyState(VK_MENU) & KEY_PRESSED) && IsInProcessedInputMode() && ServiceLocator::LocateGlobals()->getConsoleInformation()->IsAltF4CloseAllowed())
    {
        return TRUE; // let DefWindowProc generate WM_CLOSE
    }

    if ((lParam & 0x20000000) == 0)
    {   // we're iconic
        // Check for ENTER while iconic (restore accelerator).
        if (VirtualKeyCode == VK_RETURN)
        {

            return TRUE;    // call DefWindowProc
        }
        else
        {
            HandleKeyEvent(hWnd, Message, wParam, lParam, pfUnlockConsole);
            return FALSE;
        }
    }

    if (VirtualKeyCode == VK_RETURN && !bCtrlDown)
    {
        // only toggle on keydown
        if (!(lParam & KEY_TRANSITION_UP))
        {
            ServiceLocator::LocateConsoleWindow<Window>()->ToggleFullscreen();
        }

        return FALSE;
    }

    // make sure alt-space gets translated so that the system menu is displayed.
    if (!(GetKeyState(VK_CONTROL) & KEY_PRESSED))
    {
        if (VirtualKeyCode == VK_SPACE)
        {
            if (IsInVirtualTerminalInputMode())
            {
                HandleKeyEvent(hWnd, Message, wParam, lParam, pfUnlockConsole);
                return FALSE;
            }

            return TRUE;    // call DefWindowProc
        }

        if (VirtualKeyCode == VK_ESCAPE)
        {
            return TRUE;    // call DefWindowProc
        }
        if (VirtualKeyCode == VK_TAB)
        {
            return TRUE;    // call DefWindowProc
        }
    }

    HandleKeyEvent(hWnd, Message, wParam, lParam, pfUnlockConsole);

    return FALSE;
}

// Routine Description:
// - Returns TRUE if DefWindowProc should be called.
BOOL HandleMouseEvent(_In_ const SCREEN_INFORMATION * const pScreenInfo, _In_ const UINT Message, _In_ const WPARAM wParam, _In_ const LPARAM lParam)
{
    if (Message != WM_MOUSEMOVE)
    {
        // Log a telemetry flag saying the user interacted with the Console
        Telemetry::Instance().SetUserInteractive();
    }

    Selection* const pSelection = &Selection::Instance();

    if (!(ServiceLocator::LocateGlobals()->getConsoleInformation()->Flags & CONSOLE_HAS_FOCUS) && !pSelection->IsMouseButtonDown())
    {
        return TRUE;
    }

    if (ServiceLocator::LocateGlobals()->getConsoleInformation()->Flags & CONSOLE_IGNORE_NEXT_MOUSE_INPUT)
    {
        // only reset on up transition
        if (Message != WM_LBUTTONDOWN && Message != WM_MBUTTONDOWN && Message != WM_RBUTTONDOWN)
        {
            ServiceLocator::LocateGlobals()->getConsoleInformation()->Flags &= ~CONSOLE_IGNORE_NEXT_MOUSE_INPUT;
            return FALSE;
        }
        return TRUE;
    }

    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms645617(v=vs.85).aspx
    //  Important  Do not use the LOWORD or HIWORD macros to extract the x- and y-
    //  coordinates of the cursor position because these macros return incorrect
    //  results on systems with multiple monitors. Systems with multiple monitors
    //  can have negative x- and y- coordinates, and LOWORD and HIWORD treat the
    //  coordinates as unsigned quantities.
    short x = GET_X_LPARAM(lParam);
    short y = GET_Y_LPARAM(lParam);

    COORD MousePosition;
    // If it's a *WHEEL event, it's in screen coordinates, not window
    if (Message == WM_MOUSEWHEEL || Message == WM_MOUSEHWHEEL)
    {
        POINT coords = { x, y };
        ScreenToClient(ServiceLocator::LocateConsoleWindow()->GetWindowHandle(), &coords);
        MousePosition = { (SHORT)coords.x, (SHORT)coords.y };
    }
    else
    {
        MousePosition = { x, y };
    }

    // translate mouse position into characters, if necessary.
    COORD ScreenFontSize = pScreenInfo->GetScreenFontSize();
    MousePosition.X /= ScreenFontSize.X;
    MousePosition.Y /= ScreenFontSize.Y;

    const bool fShiftPressed = IsFlagSet(GetKeyState(VK_SHIFT), KEY_PRESSED);

    // We need to try and have the virtual terminal handle the mouse's position in viewport coordinates,
    //   not in screen buffer coordinates. It expects the top left to always be 0,0
    //   (the TerminalMouseInput object will add (1,1) to convert to VT coords on it's own.)
    // Mouse events with shift pressed will ignore this and fall through to the default handler.
    //   This is in line with PuTTY's behavior and vim's own documentation:
    //   "The xterm handling of the mouse buttons can still be used by keeping the shift key pressed." - `:help 'mouse'`, vim.
    // Mouse events while we're selecting or have a selection will also skip this and fall though
    //   (so that the VT handler doesn't eat any selection region updates)
    if (!fShiftPressed && !pSelection->IsInSelectingState())
    {
        short sDelta = 0;
        if (Message == WM_MOUSEWHEEL)
        {
            short sWheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
            // For most devices, we'll get mouse events as multiples of 
            // WHEEL_DELTA, where WHEEL_DELTA represents a single scroll unit
            // But sometimes, things like trackpads will scroll in finer
            // measurements. In this case, the VT mouse scrolling wouldn't work.
            // So if that happens, ensure we scroll at least one time.
            if (abs(sWheelDelta) < WHEEL_DELTA)
            {
                sDelta = sWheelDelta < 0 ? -1 : 1;
            }
            else
            {
                sDelta = sWheelDelta / WHEEL_DELTA;
            }
        }

        if (HandleTerminalMouseEvent(MousePosition, Message, GET_KEYSTATE_WPARAM(wParam), sDelta))
        {
            return FALSE;
        }
    }

    MousePosition.X += pScreenInfo->GetBufferViewport().Left;
    MousePosition.Y += pScreenInfo->GetBufferViewport().Top;

    const COORD coordScreenBufferSize = pScreenInfo->GetScreenBufferSize();

    // make sure mouse position is clipped to screen buffer
    if (MousePosition.X < 0)
    {
        MousePosition.X = 0;
    }
    else if (MousePosition.X >= coordScreenBufferSize.X)
    {
        MousePosition.X = coordScreenBufferSize.X - 1;
    }
    if (MousePosition.Y < 0)
    {
        MousePosition.Y = 0;
    }
    else if (MousePosition.Y >= coordScreenBufferSize.Y)
    {
        MousePosition.Y = coordScreenBufferSize.Y - 1;
    }

    // Process the transparency mousewheel message before the others so that we can
    // process all the mouse events within the Selection and QuickEdit check
    if (Message == WM_MOUSEWHEEL)
    {
        const short sKeyState = GET_KEYSTATE_WPARAM(wParam);

        // ctrl+shift+scroll will adjust the transparency of the window
        if ((sKeyState & MK_SHIFT) && (sKeyState & MK_CONTROL))
        {
            const short sDelta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            ServiceLocator::LocateConsoleWindow<Window>()->ChangeWindowOpacity(OPACITY_DELTA_INTERVAL * sDelta);
            ServiceLocator::LocateConsoleWindow()->SetWindowHasMoved(true);
        }
    }

    if (pSelection->IsInSelectingState() || pSelection->IsInQuickEditMode())
    {
        if (Message == WM_LBUTTONDOWN)
        {
            // make sure message matches button state
            if (!(GetKeyState(VK_LBUTTON) & KEY_PRESSED))
            {
                return FALSE;
            }

            if (pSelection->IsInQuickEditMode() && !pSelection->IsInSelectingState())
            {
                // start a mouse selection
                pSelection->InitializeMouseSelection(MousePosition);

                pSelection->MouseDown();

                // Check for ALT-Mouse Down "use alternate selection"
                // If in box mode, use line mode. If in line mode, use box mode.
                // TODO: move into initialize?
                pSelection->CheckAndSetAlternateSelection();

                pSelection->ShowSelection();
            }
            else
            {
                bool fExtendSelection = false;

                // We now capture the mouse to our Window. We do this so that the
                // user can "scroll" the selection endpoint to an off screen
                // position by moving the mouse off the client area.
                if (pSelection->IsMouseInitiatedSelection())
                {
                    // Check for SHIFT-Mouse Down "continue previous selection" command.
                    if (fShiftPressed)
                    {
                        fExtendSelection = true;
                    }
                }

                // if we chose to extend the selection, do that.
                if (fExtendSelection)
                {
                    pSelection->MouseDown();
                    pSelection->ExtendSelection(MousePosition);
                }
                else
                {
                    // otherwise, set up a new selection from here. note that it's important to ClearSelection(true) here
                    // because ClearSelection() unblocks console output, causing us to have
                    // a line of output occur every time the user changes the selection.
                    pSelection->ClearSelection(true);
                    pSelection->InitializeMouseSelection(MousePosition);
                    pSelection->MouseDown();
                    pSelection->ShowSelection();
                }
            }
        }
        else if (Message == WM_LBUTTONUP)
        {
            if (pSelection->IsInSelectingState() && pSelection->IsMouseInitiatedSelection())
            {
                pSelection->MouseUp();
            }
        }
        else if (Message == WM_LBUTTONDBLCLK)
        {
            // on double-click, attempt to select a "word" beneath the cursor
            COORD coordSelectionAnchor;
            pSelection->GetSelectionAnchor(&coordSelectionAnchor);

            if ((MousePosition.X == coordSelectionAnchor.X) && (MousePosition.Y == coordSelectionAnchor.Y))
            {
                ROW* const Row = pScreenInfo->TextInfo->GetRowByOffset(MousePosition.Y);
                ASSERT(Row != nullptr);

                while (coordSelectionAnchor.X > 0)
                {
                    if (IS_WORD_DELIM(Row->CharRow.Chars[coordSelectionAnchor.X - 1]))
                    {
                        break;
                    }
                    coordSelectionAnchor.X--;
                }
                while (MousePosition.X < coordScreenBufferSize.X)
                {
                    if (IS_WORD_DELIM(Row->CharRow.Chars[MousePosition.X]))
                    {
                        break;
                    }
                    MousePosition.X++;
                }
                if (ServiceLocator::LocateGlobals()->getConsoleInformation()->GetTrimLeadingZeros())
                {
                    // Trim the leading zeros: 000fe12 -> fe12, except 0x and 0n.
                    // Useful for debugging
                    if (MousePosition.X > coordSelectionAnchor.X + 2 &&
                        Row->CharRow.Chars[coordSelectionAnchor.X + 1] != L'x' &&
                        Row->CharRow.Chars[coordSelectionAnchor.X + 1] != L'X' && Row->CharRow.Chars[coordSelectionAnchor.X + 1] != L'n')
                    {
                        // Don't touch the selection begins with 0x
                        while (Row->CharRow.Chars[coordSelectionAnchor.X] == L'0' && coordSelectionAnchor.X < MousePosition.X - 1)
                        {
                            coordSelectionAnchor.X++;
                        }
                    }
                }

                // update both ends of the selection since we may have adjusted the anchor in some circumstances.
                pSelection->AdjustSelection(coordSelectionAnchor, MousePosition);
            }
        }
        else if ((Message == WM_RBUTTONDOWN) || (Message == WM_RBUTTONDBLCLK))
        {
            if (!pSelection->IsMouseButtonDown())
            {
                if (pSelection->IsInSelectingState())
                {
                    // Capture data on when quick edit copy is used in proc or raw mode
                    if (IsInProcessedInputMode())
                    {
                        Telemetry::Instance().LogQuickEditCopyProcUsed();
                    }
                    else
                    {
                        Telemetry::Instance().LogQuickEditCopyRawUsed();
                    }

                    Clipboard::Instance().Copy();
                }
                else if (ServiceLocator::LocateGlobals()->getConsoleInformation()->Flags & CONSOLE_QUICK_EDIT_MODE)
                {
                    // Capture data on when quick edit paste is used in proc or raw mode
                    if (IsInProcessedInputMode())
                    {
                        Telemetry::Instance().LogQuickEditPasteProcUsed();
                    }
                    else
                    {
                        Telemetry::Instance().LogQuickEditPasteRawUsed();
                    }

                    Clipboard::Instance().Paste();
                }
                ServiceLocator::LocateGlobals()->getConsoleInformation()->Flags |= CONSOLE_IGNORE_NEXT_MOUSE_INPUT;
            }
        }
        else if (Message == WM_MBUTTONDOWN)
        {
            ServiceLocator::LocateConsoleControl<Microsoft::Console::Interactivity::Win32::ConsoleControl>()
                ->EnterReaderModeHelper(ServiceLocator::LocateConsoleWindow()->GetWindowHandle());
        }
        else if (Message == WM_MOUSEMOVE)
        {
            if (pSelection->IsMouseButtonDown())
            {
                pSelection->ExtendSelection(MousePosition);
            }
        }
        else if (Message == WM_MOUSEWHEEL || Message == WM_MOUSEHWHEEL)
        {
            return TRUE;
        }

        // We're done processing the messages for selection. We need to return
        return FALSE;
    }

    if (IsFlagClear(ServiceLocator::LocateGlobals()->getConsoleInformation()->pInputBuffer->InputMode, ENABLE_MOUSE_INPUT))
    {
        ReleaseCapture();
        return TRUE;
    }

    INPUT_RECORD InputEvent;
    InputEvent.Event.MouseEvent.dwControlKeyState = GetControlKeyState(0);

    ULONG ButtonFlags;
    ULONG EventFlags;
    switch (Message)
    {
    case WM_LBUTTONDOWN:
        SetCapture(ServiceLocator::LocateConsoleWindow()->GetWindowHandle());
        ButtonFlags = FROM_LEFT_1ST_BUTTON_PRESSED;
        EventFlags = 0;
        break;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        ReleaseCapture();
        ButtonFlags = EventFlags = 0;
        break;
    case WM_RBUTTONDOWN:
        SetCapture(ServiceLocator::LocateConsoleWindow()->GetWindowHandle());
        ButtonFlags = RIGHTMOST_BUTTON_PRESSED;
        EventFlags = 0;
        break;
    case WM_MBUTTONDOWN:
        SetCapture(ServiceLocator::LocateConsoleWindow()->GetWindowHandle());
        ButtonFlags = FROM_LEFT_2ND_BUTTON_PRESSED;
        EventFlags = 0;
        break;
    case WM_MOUSEMOVE:
        ButtonFlags = 0;
        EventFlags = MOUSE_MOVED;
        break;
    case WM_LBUTTONDBLCLK:
        ButtonFlags = FROM_LEFT_1ST_BUTTON_PRESSED;
        EventFlags = DOUBLE_CLICK;
        break;
    case WM_RBUTTONDBLCLK:
        ButtonFlags = RIGHTMOST_BUTTON_PRESSED;
        EventFlags = DOUBLE_CLICK;
        break;
    case WM_MBUTTONDBLCLK:
        ButtonFlags = FROM_LEFT_2ND_BUTTON_PRESSED;
        EventFlags = DOUBLE_CLICK;
        break;
    case WM_MOUSEWHEEL:
        ButtonFlags = ((UINT)wParam & 0xFFFF0000);
        EventFlags = MOUSE_WHEELED;
        break;
    case WM_MOUSEHWHEEL:
        ButtonFlags = ((UINT)wParam & 0xFFFF0000);
        EventFlags = MOUSE_HWHEELED;
        break;
    default:
        RIPMSG1(RIP_ERROR, "Invalid message 0x%x", Message);
        ButtonFlags = 0;
        EventFlags = 0;
        break;
    }

    InputEvent.EventType = MOUSE_EVENT;
    InputEvent.Event.MouseEvent.dwMousePosition = MousePosition;
    InputEvent.Event.MouseEvent.dwEventFlags = EventFlags;
    InputEvent.Event.MouseEvent.dwButtonState = ConvertMouseButtonState(ButtonFlags, (UINT)wParam);
    ULONG const EventsWritten = ServiceLocator::LocateGlobals()->getConsoleInformation()->pInputBuffer->WriteInputBuffer(&InputEvent, 1);
    if (EventsWritten != 1)
    {
        RIPMSG1(RIP_WARNING, "PutInputInBuffer: EventsWritten != 1 (0x%x), 1 expected", EventsWritten);
    }

    return FALSE;
}

// ----------------------------
// Window Initialization
// ----------------------------

// Routine Description:
// - This routine gets called to filter input to console dialogs so that we can do the special processing that StoreKeyInfo does.
LRESULT DialogHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    MSG msg = *((PMSG)lParam);

    UNREFERENCED_PARAMETER(wParam);

    if (nCode == MSGF_DIALOGBOX)
    {
        if (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST)
        {
            if (msg.message != WM_CHAR && msg.message != WM_DEADCHAR && msg.message != WM_SYSCHAR && msg.message != WM_SYSDEADCHAR)
            {

                // don't store key info if dialog box input
                if (GetWindowLongPtrW(msg.hwnd, GWLP_HWNDPARENT) == 0)
                {
                    StoreKeyInfo(&msg);
                }
            }
        }
    }

    return 0;
}

// Routine Description:
// - This routine gets called by the console input thread to set up the console window.
NTSTATUS InitWindowsSubsystem(_Out_ HHOOK * phhook)
{
    ConsoleProcessHandle* ProcessData = ServiceLocator::LocateGlobals()->getConsoleInformation()->ProcessHandleList.FindProcessInList(ConsoleProcessList::ROOT_PROCESS_ID);
    ASSERT(ProcessData != nullptr && ProcessData->fRootProcess);

    // Create and activate the main window
    NTSTATUS Status = Window::CreateInstance(ServiceLocator::LocateGlobals()->getConsoleInformation(), ServiceLocator::LocateGlobals()->getConsoleInformation()->ScreenBuffers);

    if (!NT_SUCCESS(Status))
    {
        RIPMSG2(RIP_WARNING, "CreateWindowsWindow failed with status 0x%x, gle = 0x%x", Status, GetLastError());
        return Status;
    }

    // We intentionally ignore the return value of SetWindowsHookEx. There are mixed LUID cases where this call will fail but in the past this call
    // was special cased (for CSRSS) to always succeed. Thus, we ignore failure for app compat (as not having the hook isn't fatal).
    *phhook = SetWindowsHookExW(WH_MSGFILTER, (HOOKPROC)DialogHookProc, nullptr, GetCurrentThreadId());

    SetConsoleWindowOwner(ServiceLocator::LocateConsoleWindow()->GetWindowHandle(), ProcessData);

    ServiceLocator::LocateConsoleWindow<Window>()->ActivateAndShow(ServiceLocator::LocateGlobals()->getConsoleInformation()->GetShowWindow());

    NotifyWinEvent(EVENT_CONSOLE_START_APPLICATION, ServiceLocator::LocateConsoleWindow()->GetWindowHandle(), ProcessData->dwProcessId, 0);

    return STATUS_SUCCESS;
}

// ----------------------------
// Console Input Thread
// (for a window)
// ----------------------------

DWORD ConsoleInputThreadProcWin32(LPVOID /*lpParameter*/)
{
    InitEnvironmentVariables();

    LockConsole();
    HHOOK hhook;
    NTSTATUS Status = InitWindowsSubsystem(&hhook);
    UnlockConsole();
    if (!NT_SUCCESS(Status))
    {
        ServiceLocator::LocateGlobals()->ntstatusConsoleInputInitStatus = Status;
        ServiceLocator::LocateGlobals()->hConsoleInputInitEvent.SetEvent();
        return Status;
    }

    ServiceLocator::LocateGlobals()->hConsoleInputInitEvent.SetEvent();

    for (;;)
    {
        MSG msg;
        if (GetMessageW(&msg, nullptr, 0, 0) == 0)
        {
            break;
        }

        // --- START LOAD BEARING CODE ---
        // TranslateMessageEx appears to be necessary for a few things (that we could in the future take care of ourselves...)
        // 1. The normal TranslateMessage will return TRUE for all WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP 
        //    no matter what. 
        //    - This means that if there *is* a translation for the keydown, it will post a WM_CHAR to our queue and say TRUE.
        //      ***HOWEVER*** it also means that if there is *NOT* a translation for the keydown, it will post nothing and still say TRUE.
        //    - TRUE from TranslateMessage typically means "don't dispatch, it's already handled."
        //    - *But* the console needs to dispatch a WM_KEYDOWN that wasn't translated into a WM_CHAR so the underlying console client can 
        //      receive it and decide what to do with it.
        //    - Thus TranslateMessageEx was kludged in December 1990 to return FALSE for the case where it doesn't post a WM_CHAR so the
        //      console can know this and handle it.
        //    - Instead of using this kludge from many years ago... we could instead use the ToUnicode/ToUnicodeEx exports to translate
        //      the WM_KEYDOWN to WM_CHAR ourselves and synchronously dispatch it with all context if necessary (or continue to dispatch the
        //      WM_KEYDOWN if ToUnicode offers no translation. We would no longer need the private TranslateMessageEx (or even TranslateMessage at all).
        // 2. TranslateMessage also performs translation of ALT+NUMPAD sequences on our behalf into their corresponding character input
        //    - If we take out TranslateMessage entirely as stated in part 1, we would have to reimplement our own version of translating ALT+NUMPAD
        //      sequences at this point inside the console.
        //    - The Clipboard class (clipboard.cpp) already does the inverse of this to mock up keypad sequences for text strings pasted into the console
        //      so they can be faithfully represented as a user "typing" into the client application. The vision would be we leverage the knowledge from
        //      clipboard to build a transcoder capable of doing the reverse at this point so TranslateMessage would be completely unnecessary for us.
        // Until that future point in time.... this is LOAD BEARING CODE and should not be hastily modified or removed!
        if (!ServiceLocator::LocateConsoleControl<Microsoft::Console::Interactivity::Win32::ConsoleControl>()->TranslateMessageEx(&msg, TM_POSTCHARBREAKS))
        {
            DispatchMessageW(&msg);
        }
        // do this so that alt-tab works while journalling
        else if (msg.message == WM_SYSKEYDOWN && msg.wParam == VK_TAB && (msg.lParam & 0x20000000))
        {
            // alt is really down
            DispatchMessageW(&msg);
        }
        else
        {
            StoreKeyInfo(&msg);
        }
        // -- END LOAD BEARING CODE
    }

    // Free all resources used by this thread
    DeactivateTextServices();

    if (nullptr != hhook)
    {
        UnhookWindowsHookEx(hhook);
    }

    return 0;
}