/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"
#include "inputBuffer.hpp"
#include "dbcs.h"
#include "stream.h"

// Routine Description:
// - This routine creates an input buffer.  It allocates the circular buffer and initializes the information fields.
// Arguments:
// - cEvents - The default size of the circular buffer (in INPUT_RECORDs)
// Return Value:
INPUT_INFORMATION::INPUT_INFORMATION(_In_ ULONG cEvents)
{
    if (0 == cEvents)
    {
        cEvents = DEFAULT_NUMBER_OF_EVENTS;
    }

    // Allocate memory for circular buffer.
    ULONG uTemp;
    if (FAILED(DWordAdd(cEvents, 1, &uTemp)) || FAILED(DWordMult(sizeof(INPUT_RECORD), uTemp, &uTemp)))
    {
        cEvents = DEFAULT_NUMBER_OF_EVENTS;
    }

    ULONG const BufferSize = sizeof(INPUT_RECORD) * (cEvents + 1);
    this->InputBuffer = (PINPUT_RECORD) new BYTE[BufferSize];
    THROW_IF_NULL_ALLOC(this->InputBuffer);

    NTSTATUS Status = STATUS_SUCCESS;
    this->InputWaitEvent = g_hInputEvent.get();

    // TODO: MSFT:8805366 Is this if block still necessary?
    if (!NT_SUCCESS(Status))
    {
        delete[] this->InputBuffer;
        THROW_NTSTATUS(Status);
    }

    // initialize buffer header
    this->InputBufferSize = cEvents;
    this->InputMode = ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT | ENABLE_MOUSE_INPUT;
    this->First = (ULONG_PTR) this->InputBuffer;
    this->In = (ULONG_PTR) this->InputBuffer;
    this->Out = (ULONG_PTR) this->InputBuffer;
    this->Last = (ULONG_PTR) this->InputBuffer + BufferSize;
    this->ImeMode.Disable = FALSE;
    this->ImeMode.Unavailable = FALSE;
    this->ImeMode.Open = FALSE;
    this->ImeMode.ReadyConversion = FALSE;
    this->ImeMode.InComposition = FALSE;

    ZeroMemory(&this->ReadConInpDbcsLeadByte, sizeof(INPUT_RECORD));
    ZeroMemory(&this->WriteConInpDbcsLeadByte, sizeof(INPUT_RECORD));
}

// Routine Description:
// - This routine resets the input buffer information fields to their initial values.
// Arguments:
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
void INPUT_INFORMATION::ReinitializeInputBuffer()
{
    ResetEvent(this->InputWaitEvent);

    this->InputMode = ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT | ENABLE_MOUSE_INPUT;
    this->In = (ULONG_PTR) this->InputBuffer;
    this->Out = (ULONG_PTR) this->InputBuffer;
}

// Routine Description:
// - This routine frees the resources associated with an input buffer.
// Arguments:
// - None
// Return Value:
INPUT_INFORMATION::~INPUT_INFORMATION()
{
    // TODO: MSFT:8805366 check for null before trying to close this
    // and check that it needs to be closing it in the first place.
    CloseHandle(this->InputWaitEvent);
    delete[] this->InputBuffer;
    this->InputBuffer = nullptr;
}

// Routine Description:
// - This routine returns the number of events in the input buffer.
// Arguments:
// - pcEvents - On output contains the number of events.
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
void INPUT_INFORMATION::GetNumberOfReadyEvents(_Out_ ULONG * const pcEvents)
{
    if (this->In < this->Out)
    {
        *pcEvents = (ULONG)(this->Last - this->Out);
        *pcEvents += (ULONG)(this->In - this->First);
    }
    else
    {
        *pcEvents = (ULONG)(this->In - this->Out);
    }

    *pcEvents /= sizeof(INPUT_RECORD);
}

// Routine Description:
// - This routine removes all but the key events from the buffer.
// Arguments:
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
NTSTATUS INPUT_INFORMATION::FlushAllButKeys()
{
    if (this->In != this->Out)
    {
        ULONG BufferSize;
        // Allocate memory for temp buffer.

        // TODO: MSFT:8805366 use hresults, fix DWordMult hiding
        // non-overflow errors
        if (FAILED(DWordMult(sizeof(INPUT_RECORD), this->InputBufferSize + 1, &BufferSize)))
        {
            return STATUS_INTEGER_OVERFLOW;
        }

        PINPUT_RECORD TmpInputBuffer = (PINPUT_RECORD) new BYTE[BufferSize];
        if (TmpInputBuffer == nullptr)
        {
            return STATUS_NO_MEMORY;
        }
        PINPUT_RECORD const TmpInputBufferPtr = TmpInputBuffer;

        // copy input buffer. let ReadBuffer do any compaction work.
        ULONG NumberOfEventsRead;
        BOOL Dummy;
        NTSTATUS Status = this->ReadBuffer(TmpInputBuffer, this->InputBufferSize, &NumberOfEventsRead, TRUE, FALSE, &Dummy, TRUE);

        if (!NT_SUCCESS(Status))
        {
            delete[] TmpInputBuffer;
            return Status;
        }

        this->Out = (ULONG_PTR) this->InputBuffer;
        PINPUT_RECORD InPtr = this->InputBuffer;
        for (ULONG i = 0; i < NumberOfEventsRead; i++)
        {
            // Prevent running off the end of the buffer even though ReadBuffer will surely make this shorter than when we started.
            // We have to leave one free segment at the end for the In to point to when we're done.
            if (InPtr >= (this->InputBuffer + this->InputBufferSize - 1))
            {
                break;
            }

            if (TmpInputBuffer->EventType == KEY_EVENT)
            {
                *InPtr = *TmpInputBuffer;
                InPtr++;
            }

            TmpInputBuffer++;
        }

        this->In = (ULONG_PTR) InPtr;
        if (this->In == this->Out)
        {
            ResetEvent(this->InputWaitEvent);
        }

        delete[] TmpInputBufferPtr;
    }

    return STATUS_SUCCESS;
}

// Routine Description:
// - This routine empties the input buffer
// Arguments:
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
void INPUT_INFORMATION::FlushInputBuffer()
{
    this->In = (ULONG_PTR) this->InputBuffer;
    this->Out = (ULONG_PTR) this->InputBuffer;
    ResetEvent(this->InputWaitEvent);
}

// Routine Description:
// - This routine resizes the input buffer.
// Arguments:
// - InputInformation - Pointer to input buffer information structure.
// - Size - New size in number of events.
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
NTSTATUS INPUT_INFORMATION::SetInputBufferSize(_In_ ULONG Size)
{
#if DBG
    ULONG_PTR NumberOfEvents;
    if (this->In < this->Out)
    {
        NumberOfEvents = this->Last - this->Out;
        NumberOfEvents += this->In - this->First;
    }
    else
    {
        NumberOfEvents = this->In - this->Out;
    }
    NumberOfEvents /= sizeof(INPUT_RECORD);
#endif
    ASSERT(Size > this->InputBufferSize);

    size_t BufferSize;
    // Allocate memory for new input buffer.
    if (FAILED(SizeTMult(sizeof(INPUT_RECORD), Size + 1, &BufferSize)))
    {
        return STATUS_INTEGER_OVERFLOW;
    }

    PINPUT_RECORD const InputBuffer = (PINPUT_RECORD) new BYTE[BufferSize];
    if (InputBuffer == nullptr)
    {
        return STATUS_NO_MEMORY;
    }

    // Copy old input buffer. Let the ReadBuffer do any compaction work.
    ULONG NumberOfEventsRead;
    BOOL Dummy;
    NTSTATUS Status = this->ReadBuffer(InputBuffer, Size, &NumberOfEventsRead, TRUE, FALSE, &Dummy, TRUE);

    if (!NT_SUCCESS(Status))
    {
        delete[] InputBuffer;
        return Status;
    }
    this->Out = (ULONG_PTR) InputBuffer;
    this->In = (ULONG_PTR) InputBuffer + sizeof(INPUT_RECORD) * NumberOfEventsRead;

    // adjust pointers
    this->First = (ULONG_PTR) InputBuffer;
    this->Last = (ULONG_PTR) InputBuffer + BufferSize;

    // free old input buffer
    delete[] this->InputBuffer;
    this->InputBufferSize = Size;
    this->InputBuffer = InputBuffer;

    return Status;
}

// Routine Description:
// - This routine reads from a buffer.  It does the actual circular buffer manipulation.
// Arguments:
// - Buffer - buffer to read into
// - Length - length of buffer in events
// - EventsRead - where to store number of events read
// - Peek - if TRUE, don't remove data from buffer, just copy it.
// - StreamRead - if TRUE, events with repeat counts > 1 are returned as multiple events.  also, EventsRead == 1.
// - ResetWaitEvent - on exit, TRUE if buffer became empty.
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
NTSTATUS
INPUT_INFORMATION::ReadBuffer(_Out_writes_to_(Length, *EventsRead) PINPUT_RECORD Buffer,
                              _In_ ULONG Length,
                              _Out_ PULONG EventsRead,
                              _In_ BOOL Peek,
                              _In_ BOOL StreamRead,
                              _Out_ PBOOL ResetWaitEvent,
                             _In_ BOOLEAN Unicode)
{
    *ResetWaitEvent = FALSE;

    // If StreamRead, just return one record. If repeat count is greater than
    // one, just decrement it. The repeat count is > 1 if more than one event
    // of the same type was merged. We need to expand them back to individual
    // events here.
    if (StreamRead && ((PINPUT_RECORD) (this->Out))->EventType == KEY_EVENT)
    {

        ASSERT(Length == 1);
        ASSERT(this->In != this->Out);
        memmove((PBYTE) Buffer, (PBYTE) this->Out, sizeof(INPUT_RECORD));
        this->Out += sizeof(INPUT_RECORD);
        if (this->Last == this->Out)
        {
            this->Out = this->First;
        }

        if (this->Out == this->In)
        {
            *ResetWaitEvent = TRUE;
        }

        *EventsRead = 1;
        return STATUS_SUCCESS;
    }

    ULONG BufferLengthInBytes = Length * sizeof(INPUT_RECORD);
    ULONG TransferLength, OldTransferLength;
    ULONG Length2;
    PINPUT_RECORD BufferRecords = nullptr;
    PINPUT_RECORD QueueRecords;
    WCHAR UniChar;
    WORD EventType;

    // if in > out, buffer looks like this:
    //
    //         out     in
    //    ______ _____________
    //   |      |      |      |
    //   | free | data | free |
    //   |______|______|______|
    //
    // we transfer the requested number of events or the amount in the buffer
    if (this->In > this->Out)
    {
        if ((this->In - this->Out) > BufferLengthInBytes)
        {
            TransferLength = BufferLengthInBytes;
        }
        else
        {
            TransferLength = (ULONG) (this->In - this->Out);
        }
        if (!Unicode)
        {
            BufferLengthInBytes = 0;
            OldTransferLength = TransferLength / sizeof(INPUT_RECORD);
            BufferRecords = (PINPUT_RECORD) Buffer;
            QueueRecords = (PINPUT_RECORD) this->Out;

            while (BufferLengthInBytes < Length && OldTransferLength)
            {
                UniChar = QueueRecords->Event.KeyEvent.uChar.UnicodeChar;
                EventType = QueueRecords->EventType;
                *BufferRecords++ = *QueueRecords++;
                if (EventType == KEY_EVENT)
                {
                    if (IsCharFullWidth(UniChar))
                    {
                        BufferLengthInBytes += 2;
                    }
                    else
                    {
                        BufferLengthInBytes++;
                    }
                }
                else
                {
                    BufferLengthInBytes++;
                }

                OldTransferLength--;
            }

            ASSERT(TransferLength >= OldTransferLength * sizeof(INPUT_RECORD));
            TransferLength -= OldTransferLength * sizeof(INPUT_RECORD);
        }
        else
        {
            memmove((PBYTE) Buffer, (PBYTE) this->Out, TransferLength);
        }

        *EventsRead = TransferLength / sizeof(INPUT_RECORD);
        ASSERT(*EventsRead <= Length);

        if (!Peek)
        {
            this->Out += TransferLength;
            ASSERT(this->Out <= this->Last);
        }

        if (this->Out == this->In)
        {
            *ResetWaitEvent = TRUE;
        }

        return STATUS_SUCCESS;
    }

    // if out > in, buffer looks like this:
    //
    //         in     out
    //    ______ _____________
    //   |      |      |      |
    //   | data | free | data |
    //   |______|______|______|
    //
    // we read from the out pointer to the end of the buffer then from the
    // beginning of the buffer, until we hit the in pointer or enough bytes
    // are read.
    else
    {
        if ((this->Last - this->Out) > BufferLengthInBytes)
        {
            TransferLength = BufferLengthInBytes;
        }
        else
        {
            TransferLength = (ULONG) (this->Last - this->Out);
        }

        if (!Unicode)
        {
            BufferLengthInBytes = 0;
            OldTransferLength = TransferLength / sizeof(INPUT_RECORD);
            BufferRecords = (PINPUT_RECORD) Buffer;
            QueueRecords = (PINPUT_RECORD) this->Out;

            while (BufferLengthInBytes < Length && OldTransferLength)
            {
                UniChar = QueueRecords->Event.KeyEvent.uChar.UnicodeChar;
                EventType = QueueRecords->EventType;
                *BufferRecords++ = *QueueRecords++;
                if (EventType == KEY_EVENT)
                {
                    if (IsCharFullWidth(UniChar))
                    {
                        BufferLengthInBytes += 2;
                    }
                    else
                    {
                        BufferLengthInBytes++;
                    }
                }
                else
                {
                    BufferLengthInBytes++;
                }

                OldTransferLength--;
            }

            ASSERT(TransferLength >= OldTransferLength * sizeof(INPUT_RECORD));
            TransferLength -= OldTransferLength * sizeof(INPUT_RECORD);
        }
        else
        {
            memmove((PBYTE) Buffer, (PBYTE) this->Out, TransferLength);
        }

        *EventsRead = TransferLength / sizeof(INPUT_RECORD);
        ASSERT(*EventsRead <= Length);

        if (!Peek)
        {
            this->Out += TransferLength;
            ASSERT(this->Out <= this->Last);
            if (this->Out == this->Last)
            {
                this->Out = this->First;
            }
        }

        if (!Unicode)
        {
            if (BufferLengthInBytes >= Length)
            {
                if (this->Out == this->In)
                {
                    *ResetWaitEvent = TRUE;
                }
                return STATUS_SUCCESS;
            }
        }
        else if (*EventsRead == Length)
        {
            if (this->Out == this->In)
            {
                *ResetWaitEvent = TRUE;
            }

            return STATUS_SUCCESS;
        }

        // hit end of buffer, read from beginning
        OldTransferLength = TransferLength;
        Length2 = Length;
        if (!Unicode)
        {
            ASSERT(Length > BufferLengthInBytes);
            Length -= BufferLengthInBytes;
            if (Length == 0)
            {
                if (this->Out == this->In)
                {
                    *ResetWaitEvent = TRUE;
                }
                return STATUS_SUCCESS;
            }
            BufferLengthInBytes = Length * sizeof(INPUT_RECORD);

            if ((this->In - this->First) > BufferLengthInBytes)
            {
                TransferLength = BufferLengthInBytes;
            }
            else
            {
                TransferLength = (ULONG) (this->In - this->First);
            }
        }
        else if ((this->In - this->First) > (BufferLengthInBytes - OldTransferLength))
        {
            TransferLength = BufferLengthInBytes - OldTransferLength;
        }
        else
        {
            TransferLength = (ULONG) (this->In - this->First);
        }
        if (!Unicode)
        {
            BufferLengthInBytes = 0;
            OldTransferLength = TransferLength / sizeof(INPUT_RECORD);
            QueueRecords = (PINPUT_RECORD) this->First;

            while (BufferLengthInBytes < Length && OldTransferLength)
            {
                UniChar = QueueRecords->Event.KeyEvent.uChar.UnicodeChar;
                EventType = QueueRecords->EventType;
                *BufferRecords++ = *QueueRecords++;
                if (EventType == KEY_EVENT)
                {
                    if (IsCharFullWidth(UniChar))
                    {
                        BufferLengthInBytes += 2;
                    }
                    else
                    {
                        BufferLengthInBytes++;
                    }
                }
                else
                {
                    BufferLengthInBytes++;
                }
                OldTransferLength--;
            }

            ASSERT(TransferLength >= OldTransferLength * sizeof(INPUT_RECORD));
            TransferLength -= OldTransferLength * sizeof(INPUT_RECORD);
        }
        else
        {
            memmove((PBYTE) Buffer + OldTransferLength, (PBYTE) this->First, TransferLength);
        }

        *EventsRead += TransferLength / sizeof(INPUT_RECORD);
        ASSERT(*EventsRead <= Length2);

        if (!Peek)
        {
            this->Out = this->First + TransferLength;
        }

        if (this->Out == this->In)
        {
            *ResetWaitEvent = TRUE;
        }

        return STATUS_SUCCESS;
    }
}

// Routine Description:
// - This routine reads from the input buffer.
// Arguments:
// - pInputRecord - Buffer to read into.
// - pcLength - On input, number of events to read.  On output, number of events read.
// - fPeek - If TRUE, copy events to pInputRecord but don't remove them from the input buffer.
// - fWaitForData - if TRUE, wait until an event is input.  if FALSE, return immediately
// - fStreamRead - if TRUE, events with repeat counts > 1 are returned as multiple events.  also, EventsRead == 1.
// - pHandleData - Pointer to handle data structure.  This parameter is optional if fWaitForData is false.
// - pConsoleMsg - if called from dll (not InputThread), points to api message.  this parameter is used for wait block processing.
// - pfnWaitRoutine - Routine to call when wait is woken up.
// - pvWaitParameter - Parameter to pass to wait routine.
// - cbWaitParameter - Length of wait parameter.
// - fWaitBlockExists - TRUE if wait block has already been created.
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
NTSTATUS INPUT_INFORMATION::ReadInputBuffer(_Out_writes_(*pcLength) PINPUT_RECORD pInputRecord,
                                            _Inout_ PDWORD pcLength,
                                            _In_ BOOL const fPeek,
                                            _In_ BOOL const fWaitForData,
                                            _In_ BOOL const fStreamRead,
                                            _In_ INPUT_READ_HANDLE_DATA* pHandleData,
                                            _In_opt_ PCONSOLE_API_MSG pConsoleMsg,
                                            _In_opt_ ConsoleWaitRoutine pfnWaitRoutine,
                                            _In_reads_bytes_opt_(cbWaitParameter) PVOID pvWaitParameter,
                                            _In_ ULONG const cbWaitParameter,
                                            _In_ BOOLEAN const fWaitBlockExists,
                                            _In_ BOOLEAN const fUnicode)
{
    NTSTATUS Status;
    if (this->In == this->Out)
    {
        if (!fWaitForData)
        {
            *pcLength = 0;
            return STATUS_SUCCESS;
        }

        pHandleData->IncrementReadCount();
        Status = WaitForMoreToRead(pConsoleMsg, pfnWaitRoutine, pvWaitParameter, cbWaitParameter, fWaitBlockExists);
        if (!NT_SUCCESS(Status))
        {
            if (Status != CONSOLE_STATUS_WAIT)
            {
                // WaitForMoreToRead failed, restore ReadCount and bail out
                pHandleData->DecrementReadCount();
            }

            *pcLength = 0;
            return Status;
        }
    }

    // read from buffer
    ULONG EventsRead;
    BOOL ResetWaitEvent;
    Status = this->ReadBuffer(pInputRecord, *pcLength, &EventsRead, fPeek, fStreamRead, &ResetWaitEvent, fUnicode);
    if (ResetWaitEvent)
    {
        ResetEvent(this->InputWaitEvent);
    }

    *pcLength = EventsRead;
    return Status;
}

// Routine Description:
// - This routine writes to a buffer.  It does the actual circular buffer manipulation.
// Arguments:
// - Buffer - buffer to write from
// - Length - length of buffer in events
// - EventsWritten - where to store number of events written.
// - SetWaitEvent - on exit, TRUE if buffer became non-empty.
// Return Value:
// - ERROR_BROKEN_PIPE - no more readers.
// Note:
// - The console lock must be held when calling this routine.
NTSTATUS INPUT_INFORMATION::WriteBuffer(_In_ PVOID Buffer, _In_ ULONG Length, _Out_ PULONG EventsWritten, _Out_ PBOOL SetWaitEvent)
{
    NTSTATUS Status;
    ULONG TransferLength;
    ULONG BufferLengthInBytes;

    *SetWaitEvent = FALSE;

    // windows sends a mouse_move message each time a window is updated.
    // coalesce these.
    if (Length == 1 && this->Out != this->In)
    {
        PINPUT_RECORD InputEvent = (PINPUT_RECORD) Buffer;

        // this bit coalesces mouse moved events (updating the (x, y)
        // positions of the event already in the buffer)
        if (InputEvent->EventType == MOUSE_EVENT && InputEvent->Event.MouseEvent.dwEventFlags == MOUSE_MOVED)
        {
            PINPUT_RECORD LastInputEvent;

            if (this->In == this->First)
            {
                LastInputEvent = (PINPUT_RECORD) (this->Last - sizeof(INPUT_RECORD));
            }
            else
            {
                LastInputEvent = (PINPUT_RECORD) (this->In - sizeof(INPUT_RECORD));
            }
            if (LastInputEvent->EventType == MOUSE_EVENT && LastInputEvent->Event.MouseEvent.dwEventFlags == MOUSE_MOVED)
            {
                LastInputEvent->Event.MouseEvent.dwMousePosition.X = InputEvent->Event.MouseEvent.dwMousePosition.X;
                LastInputEvent->Event.MouseEvent.dwMousePosition.Y = InputEvent->Event.MouseEvent.dwMousePosition.Y;
                *EventsWritten = 1;
                return STATUS_SUCCESS;
            }
        }
        // this bit coalesces key events (upping the repeat count of
        // the event already in the buffer)
        else if (InputEvent->EventType == KEY_EVENT && InputEvent->Event.KeyEvent.bKeyDown)
        {
            PINPUT_RECORD LastInputEvent;
            if (this->In == this->First)
            {
                LastInputEvent = (PINPUT_RECORD) (this->Last - sizeof(INPUT_RECORD));
            }
            else
            {
                LastInputEvent = (PINPUT_RECORD) (this->In - sizeof(INPUT_RECORD));
            }

            // don't coalesce the key event if it's a full width char
            if (IsCharFullWidth(InputEvent->Event.KeyEvent.uChar.UnicodeChar))
            {
                /* do nothing */ ;
            }
            else if (InputEvent->Event.KeyEvent.dwControlKeyState & NLS_IME_CONVERSION)
            {
                if (LastInputEvent->EventType == KEY_EVENT &&
                    LastInputEvent->Event.KeyEvent.bKeyDown &&
                    (LastInputEvent->Event.KeyEvent.uChar.UnicodeChar == InputEvent->Event.KeyEvent.uChar.UnicodeChar) &&
                    (LastInputEvent->Event.KeyEvent.dwControlKeyState == InputEvent->Event.KeyEvent.dwControlKeyState))
                {
                    LastInputEvent->Event.KeyEvent.wRepeatCount += InputEvent->Event.KeyEvent.wRepeatCount;
                    *EventsWritten = 1;
                    return STATUS_SUCCESS;
                }
            }
            // this one checks that the scan code is the same and the
            // one above doesn't.
            else if (LastInputEvent->EventType == KEY_EVENT &&
                     LastInputEvent->Event.KeyEvent.bKeyDown &&
                     (LastInputEvent->Event.KeyEvent.wVirtualScanCode ==  InputEvent->Event.KeyEvent.wVirtualScanCode) && // scancode same
                     (LastInputEvent->Event.KeyEvent.uChar.UnicodeChar == InputEvent->Event.KeyEvent.uChar.UnicodeChar) && // character same
                     (LastInputEvent->Event.KeyEvent.dwControlKeyState == InputEvent->Event.KeyEvent.dwControlKeyState)) // ctrl/alt/shift state same
            {
                LastInputEvent->Event.KeyEvent.wRepeatCount += InputEvent->Event.KeyEvent.wRepeatCount;
                *EventsWritten = 1;
                return STATUS_SUCCESS;
            }
        }
    }

    BufferLengthInBytes = Length * sizeof(INPUT_RECORD);
    *EventsWritten = 0;
    while (*EventsWritten < Length)
    {

        // if out > in, buffer looks like this:
        //
        //             in     out
        //        ______ _____________
        //       |      |      |      |
        //       | data | free | data |
        //       |______|______|______|
        //
        // we can write from in to out-1
        if (this->Out > this->In)
        {
            TransferLength = BufferLengthInBytes;
            // check if we need to grow the input buffer size
            if ((this->Out - this->In - sizeof(INPUT_RECORD)) < BufferLengthInBytes)
            {
                Status = this->SetInputBufferSize(this->InputBufferSize + Length + INPUT_BUFFER_SIZE_INCREMENT);
                if (!NT_SUCCESS(Status))
                {
                    RIPMSG1(RIP_WARNING, "Couldn't grow input buffer, Status == 0x%x", Status);
                    TransferLength = (ULONG) (this->Out - this->In - sizeof(INPUT_RECORD));
                    if (TransferLength == 0)
                    {
                        return Status;
                    }
                }
                else
                {
                    goto OutPath;   // after resizing, in > out
                }
            }
            memmove((PBYTE) this->In, (PBYTE) Buffer, TransferLength);
            Buffer = (PVOID) (((PBYTE) Buffer) + TransferLength);
            *EventsWritten += TransferLength / sizeof(INPUT_RECORD);
            BufferLengthInBytes -= TransferLength;
            this->In += TransferLength;
        }

        // if in >= out, buffer looks like this:
        //
        //             out     in
        //        ______ _____________
        //       |      |      |      |
        //       | free | data | free |
        //       |______|______|______|
        //
        // we write from the in pointer to the end of the buffer then from the
        // beginning of the buffer, until we hit the out pointer or enough bytes
        // are written.
        else
        {
            // check if we started out with an empty buffer
            if (this->Out == this->In)
            {
                *SetWaitEvent = TRUE;
            }
OutPath:
            if ((this->Last - this->In) > BufferLengthInBytes)
            {
                TransferLength = BufferLengthInBytes;
            }
            else
            {
                // buffer is totally full
                if (this->First == this->Out && this->In == (this->Last - sizeof(INPUT_RECORD)))
                {
                    TransferLength = BufferLengthInBytes;
                    Status = this->SetInputBufferSize(this->InputBufferSize + Length + INPUT_BUFFER_SIZE_INCREMENT);
                    if (!NT_SUCCESS(Status))
                    {
                        RIPMSG1(RIP_WARNING, "Couldn't grow input buffer, Status == 0x%x", Status);
                        return Status;
                    }
                }
                else
                {
                    TransferLength = (ULONG) (this->Last - this->In);
                    if (this->First == this->Out)
                    {
                        TransferLength -= sizeof(INPUT_RECORD);
                    }
                }
            }
            memmove((PBYTE) this->In, (PBYTE) Buffer, TransferLength);
            Buffer = (PVOID) (((PBYTE) Buffer) + TransferLength);
            *EventsWritten += TransferLength / sizeof(INPUT_RECORD);
            BufferLengthInBytes -= TransferLength;
            this->In += TransferLength;
            if (this->In == this->Last)
            {
                this->In = this->First;
            }
        }
        if (TransferLength == 0)
        {
            ASSERT(FALSE);
        }
    }
    return STATUS_SUCCESS;
}

// Routine Description:
// - This routine processes special characters in the input stream.
// Arguments:
// - Console - Pointer to console structure.
// - InputEvent - Buffer to write from.
// - nLength - Number of events to write.
// Return Value:
// - Number of events to write after special characters have been stripped.
// Note:
// - The console lock must be held when calling this routine.
DWORD INPUT_INFORMATION::PreprocessInput(_In_ PINPUT_RECORD InputEvent, _In_ DWORD nLength)
{
    for (ULONG NumEvents = nLength; NumEvents != 0; NumEvents--)
    {
        if (InputEvent->EventType == KEY_EVENT && InputEvent->Event.KeyEvent.bKeyDown)
        {
            // if output is suspended, any keyboard input releases it.
            if ((g_ciConsoleInformation.Flags & CONSOLE_SUSPENDED) && !IsSystemKey(InputEvent->Event.KeyEvent.wVirtualKeyCode))
            {

                UnblockWriteConsole(CONSOLE_OUTPUT_SUSPENDED);
                memmove(InputEvent, InputEvent + 1, (NumEvents - 1) * sizeof(INPUT_RECORD));
                nLength--;
                continue;
            }

            // intercept control-s
            if ((this->InputMode & ENABLE_LINE_INPUT) &&
                (InputEvent->Event.KeyEvent.wVirtualKeyCode == VK_PAUSE || IsPauseKey(&InputEvent->Event.KeyEvent)))
            {

                g_ciConsoleInformation.Flags |= CONSOLE_OUTPUT_SUSPENDED;
                memmove(InputEvent, InputEvent + 1, (NumEvents - 1) * sizeof(INPUT_RECORD));
                nLength--;
                continue;
            }
        }
        InputEvent++;
    }
    return nLength;
}

// Routine Description:
// -  This routine writes to the beginning of the input buffer.
// Arguments:
// - pInputRecord - Buffer to write from.
// - cInputRecords - On input, number of events to write.  On output, number of events written.
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
NTSTATUS INPUT_INFORMATION::PrependInputBuffer(_In_ PINPUT_RECORD pInputRecord, _Inout_ DWORD * const pcLength)
{
    DWORD cInputRecords = *pcLength;
    cInputRecords = PreprocessInput(pInputRecord, cInputRecords);
    if (cInputRecords == 0)
    {
        return STATUS_SUCCESS;
    }

    ULONG NumExistingEvents;
    this->GetNumberOfReadyEvents(&NumExistingEvents);

    PINPUT_RECORD pExistingEvents;
    ULONG EventsRead = 0;
    BOOL Dummy;
    if (NumExistingEvents)
    {
        ULONG NumBytes;

        if (FAILED(ULongMult(NumExistingEvents, sizeof(INPUT_RECORD), &NumBytes)))
        {
            return STATUS_INTEGER_OVERFLOW;
        }

        pExistingEvents = (PINPUT_RECORD) new BYTE[NumBytes];
        if (pExistingEvents == nullptr)
        {
            return STATUS_NO_MEMORY;
        }

        NTSTATUS Status = this->ReadBuffer(pExistingEvents, NumExistingEvents, &EventsRead, FALSE, FALSE, &Dummy, TRUE);
        if (!NT_SUCCESS(Status))
        {
            delete[] pExistingEvents;
            return Status;
        }
    }
    else
    {
        pExistingEvents = nullptr;
    }

    ULONG EventsWritten;
    BOOL SetWaitEvent;
    // write new info to buffer
    this->WriteBuffer(pInputRecord, cInputRecords, &EventsWritten, &SetWaitEvent);

    // Write existing info to buffer.
    if (pExistingEvents)
    {
        this->WriteBuffer(pExistingEvents, EventsRead, &EventsWritten, &Dummy);
        delete[] pExistingEvents;
    }

    if (SetWaitEvent)
    {
        SetEvent(this->InputWaitEvent);
    }

    // alert any writers waiting for space
    this->WakeUpReadersWaitingForData();

    *pcLength = cInputRecords;
    return STATUS_SUCCESS;
}

// Routine Description:
// - This routine writes to the input buffer.
// Arguments:
// - pInputRecord - Buffer to write from.
// - cInputRecords - On input, number of events to write.
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
DWORD INPUT_INFORMATION::WriteInputBuffer(_In_ PINPUT_RECORD pInputRecord, _In_ DWORD cInputRecords)
{
    cInputRecords = PreprocessInput(pInputRecord, cInputRecords);
    if (cInputRecords == 0)
    {
        return 0;
    }

    // Write to buffer.
    ULONG EventsWritten;
    BOOL SetWaitEvent;
    this->WriteBuffer(pInputRecord, cInputRecords, &EventsWritten, &SetWaitEvent);

    if (SetWaitEvent)
    {
        SetEvent(this->InputWaitEvent);
    }

    // Alert any writers waiting for space.
    this->WakeUpReadersWaitingForData();

    return EventsWritten;
}

// Routine Description:
// - This routine wakes up readers waiting for data to read.
// Arguments:
// Return Value:
// - TRUE - The operation was successful
// - FALSE/nullptr - The operation failed.
void INPUT_INFORMATION::WakeUpReadersWaitingForData()
{
    this->WaitQueue.NotifyWaiters(false);
}