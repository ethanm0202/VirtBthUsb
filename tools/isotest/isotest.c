/* Stage 2 WinUSB isochronous geometry measurement harness. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")

DEFINE_GUID(GUID_DEVINTERFACE_ISOTEST,
    0xd3c8b701, 0x150a, 0x4e2f, 0xa6, 0x9d, 0xb7, 0xe5, 0x54, 0x24, 0xd1, 0xe2);

#define EXIT_OK 0
#define EXIT_ERROR 1
#define EXIT_DEVICE_NOT_FOUND 2
#define EP_ISOCH_OUT 0x03u
#define EP_ISOCH_IN 0x83u
#define ALT_COUNT 7u
#define HOLD_NEXT 0xAAu
#define RELEASE_HELD 0xABu
#define IO_TIMEOUT_MS 5000u
#define DMA_BUFFER_SIZE 65536u
#define HARNESS_BUILD_STAMP __DATE__ " " __TIME__

static const USHORT g_AltPacketSize[ALT_COUNT] = { 0, 9, 17, 25, 33, 49, 63 };
static const ULONG g_PacketCounts[] = { 1, 2, 4, 8, 10, 16, 20, 32, 64 };
#define PACKET_COUNT_COUNT ARRAYSIZE(g_PacketCounts)
#define MAX_CELLS ((ALT_COUNT - 1u) * 2u * PACKET_COUNT_COUNT * 4u)

typedef struct _CELL {
    UCHAR Alt;
    UCHAR Endpoint;
    ULONG Packets;
    ULONG PacketSize;
    ULONG TotalBytes;
    const char *Category;
    const char *FailedAt;
    const char *DeviceSpeed;
    DWORD Win32Error;
    ULONG UsbdStatus;
    BOOL UsbdStatusObserved;
    ULONG BytesTransferred;
    BOOL Accepted;
} CELL;

typedef enum _CANCEL_PROBE_VERDICT {
    CANCEL_PROBE_NOT_RUN = 0,
    CANCEL_PROBE_PASSED,
    CANCEL_PROBE_INCONCLUSIVE,
    CANCEL_PROBE_FAILED
} CANCEL_PROBE_VERDICT;

static CELL g_Cells[MAX_CELLS];
static ULONG g_CellCount;

static BOOL FindDevicePath(WCHAR *path, DWORD pathChars, DWORD *error)
{
    HDEVINFO info;
    SP_DEVICE_INTERFACE_DATA iface;
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = NULL;
    DWORD bytes = 0;
    BOOL found = FALSE;

    *error = ERROR_SUCCESS;
    info = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_ISOTEST, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return FALSE;
    }

    ZeroMemory(&iface, sizeof(iface));
    iface.cbSize = sizeof(iface);
    if (!SetupDiEnumDeviceInterfaces(info, NULL, &GUID_DEVINTERFACE_ISOTEST, 0, &iface)) {
        *error = GetLastError();
        goto done;
    }

    if (SetupDiGetDeviceInterfaceDetailW(info, &iface, NULL, 0, &bytes, NULL) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(*detail)) {
        *error = GetLastError();
        goto done;
    }

    detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(bytes);
    if (detail == NULL) {
        *error = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    ZeroMemory(detail, bytes);
    detail->cbSize = sizeof(*detail);
    if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, bytes, NULL, NULL)) {
        *error = GetLastError();
        goto done;
    }
    if (wcsncpy_s(path, pathChars, detail->DevicePath, _TRUNCATE) != 0) {
        *error = ERROR_INSUFFICIENT_BUFFER;
        goto done;
    }
    found = TRUE;

done:
    free(detail);
    SetupDiDestroyDeviceInfoList(info);
    return found;
}

static void AddCell(UCHAR alt, UCHAR endpoint, ULONG packets, ULONG packetSize,
                    const char *category)
{
    CELL *cell = &g_Cells[g_CellCount++];
    ZeroMemory(cell, sizeof(*cell));
    cell->Alt = alt;
    cell->Endpoint = endpoint;
    cell->Packets = packets;
    cell->PacketSize = packetSize;
    cell->TotalBytes = packets * packetSize;
    cell->Category = category;
    cell->FailedAt = "NONE";
    cell->DeviceSpeed = "UNKNOWN";
}

static void PopulatePlan(void)
{
    UCHAR alt;
    UCHAR direction;
    ULONG countIndex;

    g_CellCount = 0;
    /* Alt 0 has no usable pipes. Live mode verifies that fact but submits no transfer. */
    for (alt = 1; alt < ALT_COUNT; ++alt) {
        for (direction = 0; direction < 2; ++direction) {
            UCHAR endpoint = direction == 0 ? EP_ISOCH_OUT : EP_ISOCH_IN;
            for (countIndex = 0; countIndex < PACKET_COUNT_COUNT; ++countIndex) {
                ULONG packets = g_PacketCounts[countIndex];
                ULONG exact = g_AltPacketSize[alt];
                AddCell(alt, endpoint, packets, exact, "exact");
                AddCell(alt, endpoint, packets, exact / 2u, "sub");
                AddCell(alt, endpoint, packets, exact + 1u, "oversize");
                AddCell(alt, endpoint, packets, 0, "zero");
            }
        }
    }
}

static void PrintCellHeader(FILE *out, BOOL plan)
{
    UNREFERENCED_PARAMETER(plan);
    fprintf(out, "Alt,Direction,Endpoint,INPacketCountArgument,NominalBytesPerInterval,TotalLength,Category,FailedAt,DeviceSpeed,Win32Error,UsbdStatus,BytesTransferred,Result\n");
}

static void PrintOutConfirmationLegend(FILE *out)
{
    fprintf(out, "[*] Legend: Accepted OUT rows are confirmed only device-side; WinUSB does not report\n"
                 "[*] per-packet status or transfer byte counts for ASAP writes (UsbdStatus and BytesTransferred\n"
                 "[*] are NOT_REPORTED). Device-side confirmation lives in the isotest driver's IsochUrbCount,\n"
                 "[*] LastIsoch* values, and IsochUrbHistory under\n"
                 "[*] HKLM\\SYSTEM\\CurrentControlSet\\Services\\DeckBtIsoTest\\Parameters.\n");
}

static void PrintCells(FILE *out, BOOL plan)
{
    ULONG i;
    PrintCellHeader(out, plan);
    for (i = 0; i < g_CellCount; ++i) {
        const CELL *c = &g_Cells[i];
        fprintf(out, "%u,%s,0x%02X,", (unsigned)c->Alt,
                c->Endpoint == EP_ISOCH_IN ? "IN" : "OUT", (unsigned)c->Endpoint);
        if (c->Endpoint == EP_ISOCH_IN) fprintf(out, "%lu,", c->Packets);
        else fprintf(out, "N/A,");
        fprintf(out, "%lu,%lu,%s,%s,%s,",
                c->PacketSize, c->TotalBytes, c->Category,
                c->FailedAt != NULL ? c->FailedAt : "NONE",
                c->DeviceSpeed != NULL ? c->DeviceSpeed : "UNKNOWN");
        if (plan) {
            fprintf(out, ",,,PLANNED\n");
        } else {
            if (c->Win32Error == ERROR_SUCCESS) fprintf(out, "0,");
            else fprintf(out, "0x%08lX,", c->Win32Error);
            if (c->Accepted && c->Endpoint == EP_ISOCH_OUT) {
                fprintf(out, "NOT_REPORTED,NOT_REPORTED,ACCEPTED\n");
            } else {
                if (c->UsbdStatusObserved) fprintf(out, "0x%08lX,", c->UsbdStatus);
                else fprintf(out, "UNOBSERVED,");
                fprintf(out, "%lu,%s\n", c->BytesTransferred,
                        c->Accepted ? "ACCEPTED" : "REJECTED");
            }
        }
    }
    fprintf(out, "# Harness build: " HARNESS_BUILD_STAMP "\n");
}

typedef struct _MEASUREMENT_SUMMARY_ROW {
    const char *FailedAt;
    DWORD Win32Error;
    ULONG Count;
} MEASUREMENT_SUMMARY_ROW;

static void PrintMeasurementSummary(FILE *out, BOOL frameNumberAvailable)
{
    MEASUREMENT_SUMMARY_ROW rows[64];
    ULONG rowCount = 0;
    ULONG acceptedCount = 0;
    ULONG rejectedCount = 0;
    ULONG acceptedInCount = 0;
    ULONG acceptedOutCount = 0;
    ULONG inExactAcceptedCount = 0;
    ULONG inExactMismatchCount = 0;
    UCHAR firstMismatchAlt = 0;
    ULONG firstMismatchPackets = 0;
    ULONG firstMismatchExpected = 0;
    ULONG firstMismatchActual = 0;
    ULONG completionCount = 0;
    ULONG submitTransferCount = 0;
    ULONG registerIsochBufferCount = 0;
    ULONG submissionRejectedCount = 0;
    ULONG i;
    ULONG j;

    ZeroMemory(rows, sizeof(rows));
    for (i = 0; i < g_CellCount; ++i) {
        const CELL *c = &g_Cells[i];
        const char *failedAt = c->FailedAt != NULL ? c->FailedAt : "UNKNOWN";
        BOOL found = FALSE;

        if (c->Accepted) {
            acceptedCount++;
            if (c->Endpoint == EP_ISOCH_IN) {
                acceptedInCount++;
                if (strcmp(c->Category, "exact") == 0) {
                    ULONG expectedBytes = c->Packets * c->PacketSize;
                    inExactAcceptedCount++;
                    if (c->BytesTransferred != expectedBytes) {
                        if (inExactMismatchCount == 0) {
                            firstMismatchAlt = c->Alt;
                            firstMismatchPackets = c->Packets;
                            firstMismatchExpected = expectedBytes;
                            firstMismatchActual = c->BytesTransferred;
                        }
                        inExactMismatchCount++;
                    }
                }
            } else {
                acceptedOutCount++;
            }
        } else {
            rejectedCount++;
        }

        if (strcmp(failedAt, "COMPLETION") == 0 || c->Accepted) {
            completionCount++;
        } else if (strcmp(failedAt, "SUBMIT_TRANSFER") == 0) {
            submitTransferCount++;
        } else if (strcmp(failedAt, "REGISTER_ISOCH_BUFFER") == 0) {
            registerIsochBufferCount++;
        }

        for (j = 0; j < rowCount; ++j) {
            if (strcmp(rows[j].FailedAt, failedAt) == 0 &&
                rows[j].Win32Error == c->Win32Error) {
                rows[j].Count++;
                found = TRUE;
                break;
            }
        }
        if (!found && rowCount < ARRAYSIZE(rows)) {
            rows[rowCount].FailedAt = failedAt;
            rows[rowCount].Win32Error = c->Win32Error;
            rows[rowCount].Count = 1;
            rowCount++;
        }
    }

    submissionRejectedCount = submitTransferCount + registerIsochBufferCount;

    fprintf(out, "\n[*] --- Measurement Summary (%lu cells: %lu accepted, %lu rejected) ---\n",
            g_CellCount, acceptedCount, rejectedCount);
    fprintf(out, "%-26s %-14s %s\n", "FailedAt", "Win32Error", "Count");
    fprintf(out, "%-26s %-14s %s\n", "--------------------------", "--------------", "-----");
    for (i = 0; i < rowCount; ++i) {
        if (rows[i].Win32Error == ERROR_SUCCESS) {
            fprintf(out, "%-26s 0              %lu\n",
                    rows[i].FailedAt, rows[i].Count);
        } else {
            fprintf(out, "%-26s 0x%08lX     %lu\n",
                    rows[i].FailedAt, rows[i].Win32Error, rows[i].Count);
        }
    }
    fprintf(out, "[*] --------------------------------------------------------------------\n");
    fprintf(out, "[*] Completion vs submission split: %lu reached COMPLETION, %lu rejected at SUBMIT_TRANSFER or REGISTER_ISOCH_BUFFER (%lu SUBMIT_TRANSFER, %lu REGISTER_ISOCH_BUFFER).\n",
            completionCount, submissionRejectedCount, submitTransferCount, registerIsochBufferCount);
    fprintf(out, "[*] Accepted: %lu IN (byte counts observed), %lu OUT (byte counts not reported by WinUSB)\n",
            acceptedInCount, acceptedOutCount);
    if (inExactMismatchCount > 0) {
        fprintf(out, "[*] Observation - IN byte-count mismatch: %lu cell(s) (example: expected %lu, actual %lu, alt %u, %lu packets)\n",
                inExactMismatchCount, firstMismatchExpected, firstMismatchActual,
                (unsigned)firstMismatchAlt, firstMismatchPackets);
    } else {
        fprintf(out, "[*] Observation - IN byte-count mismatch: 0 (%lu accepted exact IN cells matched expected Packets * NominalBytesPerInterval)\n",
                inExactAcceptedCount);
    }
    if (acceptedCount == 0) {
        fprintf(out, "[*] Zero cells accepted: no USBD_STATUS was observed and therefore nothing reached the device's isochronous endpoints.\n");
    }
    fprintf(out, "[*] Frame number available: %s\n", frameNumberAvailable ? "yes" : "no");
    PrintOutConfirmationLegend(out);
    fprintf(out, "[*] Harness build: " HARNESS_BUILD_STAMP "\n");
    fprintf(out, "[*] --------------------------------------------------------------------\n\n");
}

static BOOL WriteCsv(const WCHAR *path, BOOL plan)
{
    FILE *out = NULL;
    errno_t err = _wfopen_s(&out, path, L"w");
    if (err != 0 || out == NULL) {
        fwprintf(stderr, L"[-] Cannot open CSV '%ls' (errno %d).\n", path, (int)err);
        return FALSE;
    }
    PrintCells(out, plan);
    if (fclose(out) != 0) {
        fwprintf(stderr, L"[-] Failed while closing CSV '%ls'.\n", path);
        return FALSE;
    }
    wprintf(L"[+] Wrote CSV: %ls\n", path);
    return TRUE;
}

static BOOL QueryExpectedPipes(WINUSB_INTERFACE_HANDLE iface, UCHAR alt, BOOL requireUsablePipes)
{
    USB_INTERFACE_DESCRIPTOR descriptor;
    UCHAR index;
    BOOL sawOut = FALSE;
    BOOL sawIn = FALSE;

    if (!WinUsb_QueryInterfaceSettings(iface, alt, &descriptor)) {
        printf("[-] QueryInterfaceSettings(alt %u) failed: Win32 %lu.\n",
               (unsigned)alt, GetLastError());
        return FALSE;
    }
    /* The frozen alt 0 descriptor really contains two zero-sized endpoints. */
    if (descriptor.bInterfaceNumber != 1 || descriptor.bAlternateSetting != alt ||
        descriptor.bNumEndpoints != 2) {
        printf("[-] Alt %u descriptor mismatch: interface=%u setting=%u endpoints=%u.\n",
               (unsigned)alt, (unsigned)descriptor.bInterfaceNumber,
               (unsigned)descriptor.bAlternateSetting, (unsigned)descriptor.bNumEndpoints);
        return FALSE;
    }
    for (index = 0; index < descriptor.bNumEndpoints; ++index) {
        WINUSB_PIPE_INFORMATION_EX pipe;
        if (!WinUsb_QueryPipeEx(iface, alt, index, &pipe)) {
            DWORD queryError = GetLastError();
            if (!requireUsablePipes) {
                printf("[*] Alt 0 zero-bandwidth pipe index %u is not exposed by WinUSB (Win32 %lu); recorded as an observation.\n",
                       (unsigned)index, queryError);
                return TRUE;
            }
            printf("[-] QueryPipeEx(alt %u, index %u) failed: Win32 %lu.\n",
                   (unsigned)alt, (unsigned)index, queryError);
            return FALSE;
        }
        if (pipe.PipeType != UsbdPipeTypeIsochronous ||
            pipe.MaximumPacketSize != g_AltPacketSize[alt] ||
            pipe.MaximumBytesPerInterval != g_AltPacketSize[alt] || pipe.Interval < 4) {
            printf("[-] Alt %u pipe 0x%02X mismatch: type=%u maxPacket=%u maxInterval=%lu interval=%u.\n",
                   (unsigned)alt, (unsigned)pipe.PipeId, (unsigned)pipe.PipeType,
                   (unsigned)pipe.MaximumPacketSize, pipe.MaximumBytesPerInterval,
                   (unsigned)pipe.Interval);
            return FALSE;
        }
        if (pipe.PipeId == EP_ISOCH_OUT) sawOut = TRUE;
        if (pipe.PipeId == EP_ISOCH_IN) sawIn = TRUE;
    }
    return sawOut && sawIn;
}

static BOOL DrainOverlapped(HANDLE device, WINUSB_INTERFACE_HANDLE iface,
                            OVERLAPPED *ov, DWORD *bytes, DWORD *error)
{
    DWORD wait = WaitForSingleObject(ov->hEvent, IO_TIMEOUT_MS);
    if (wait == WAIT_OBJECT_0) {
        if (!WinUsb_GetOverlappedResult(iface, ov, bytes, FALSE)) {
            *error = GetLastError();
            return FALSE;
        }
        return TRUE;
    }

    {
        DWORD waitError = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT :
                          (wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE);
        DWORD cancelError = ERROR_SUCCESS;
        if (!CancelIoEx(device, ov) && GetLastError() != ERROR_NOT_FOUND) {
            cancelError = GetLastError();
        }
        wait = WaitForSingleObject(ov->hEvent, IO_TIMEOUT_MS);
        if (wait != WAIT_OBJECT_0) {
            fprintf(stderr, "[-] FATAL: incomplete I/O did not drain after cancellation; terminating without freeing live buffers.\n");
            fflush(NULL);
            ExitProcess(EXIT_ERROR);
        }
        (void)WinUsb_GetOverlappedResult(iface, ov, bytes, FALSE);
        *error = cancelError == ERROR_SUCCESS ? waitError : cancelError;
        return FALSE;
    }
}

static void RunCell(HANDLE device, WINUSB_INTERFACE_HANDLE iface, CELL *cell,
                    UCHAR *buffer, ULONG bufferSize)
{
    WINUSB_ISOCH_BUFFER_HANDLE registered = NULL;
    USBD_ISO_PACKET_DESCRIPTOR *packets = NULL;
    OVERLAPPED ov;
    ULONG registrationLength;
    DWORD bytes = 0;
    DWORD error = ERROR_SUCCESS;
    BOOL submitted;
    BOOL completed = FALSE;
    ULONG i;

    /*
     * For IN transfers, WinUSB requires the registered isoch buffer to be sized for
     * Packets * wMaxPacketSize (a multiple of MaximumPacketSize).
     * For OUT transfers, the registered buffer must hold TotalBytes (at least 1 byte for zero-length).
     */
    if (cell->Endpoint == EP_ISOCH_IN) {
        registrationLength = cell->Packets * (ULONG)g_AltPacketSize[cell->Alt];
    } else {
        registrationLength = cell->TotalBytes == 0 ? 1 : cell->TotalBytes;
    }

    if (registrationLength > bufferSize) {
        cell->FailedAt = "HARNESS_PRECHECK";
        cell->Win32Error = ERROR_INSUFFICIENT_BUFFER;
        cell->Accepted = FALSE;
        return;
    }
    if (cell->Endpoint == EP_ISOCH_IN) {
        packets = (USBD_ISO_PACKET_DESCRIPTOR *)calloc(cell->Packets, sizeof(*packets));
        if (packets == NULL) {
            cell->FailedAt = "HARNESS_PRECHECK";
            cell->Win32Error = ERROR_NOT_ENOUGH_MEMORY;
            cell->Accepted = FALSE;
            return;
        }
    }
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ov.hEvent == NULL) {
        cell->FailedAt = "HARNESS_PRECHECK";
        cell->Win32Error = GetLastError();
        if (cell->Win32Error == ERROR_SUCCESS) {
            cell->Win32Error = ERROR_GEN_FAILURE;
        }
        cell->Accepted = FALSE;
        free(packets);
        return;
    }
    if (!WinUsb_RegisterIsochBuffer(iface, cell->Endpoint, buffer, registrationLength, &registered)) {
        cell->FailedAt = "REGISTER_ISOCH_BUFFER";
        cell->Win32Error = GetLastError();
        if (cell->Win32Error == ERROR_SUCCESS) {
            cell->Win32Error = ERROR_GEN_FAILURE;
        }
        cell->Accepted = FALSE;
        goto done;
    }

    if (cell->Endpoint == EP_ISOCH_IN) {
        submitted = WinUsb_ReadIsochPipeAsap(registered, 0, cell->TotalBytes, FALSE,
                                              cell->Packets, packets, &ov);
    } else {
        submitted = WinUsb_WriteIsochPipeAsap(registered, 0, cell->TotalBytes, FALSE, &ov);
    }
    if (submitted) {
        completed = DrainOverlapped(device, iface, &ov, &bytes, &error);
        if (!completed) {
            cell->FailedAt = "COMPLETION";
        }
    } else {
        error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            error = ERROR_SUCCESS;
            completed = DrainOverlapped(device, iface, &ov, &bytes, &error);
            if (!completed) {
                cell->FailedAt = "COMPLETION";
            }
        } else {
            cell->FailedAt = "SUBMIT_TRANSFER";
        }
    }

    cell->Win32Error = completed ? ERROR_SUCCESS : error;
    cell->BytesTransferred = bytes;
    cell->Accepted = completed;
    if (completed) {
        cell->FailedAt = "NONE";
        if (packets != NULL) {
            ULONG descriptorBytes = 0;
            cell->UsbdStatusObserved = TRUE;
            cell->UsbdStatus = USBD_STATUS_SUCCESS;
            for (i = 0; i < cell->Packets; ++i) {
                descriptorBytes += packets[i].Length;
                if (packets[i].Status != USBD_STATUS_SUCCESS) {
                    cell->UsbdStatus = packets[i].Status;
                    cell->Accepted = FALSE;
                    cell->FailedAt = "COMPLETION";
                }
            }
            cell->BytesTransferred = descriptorBytes;
            if (!cell->Accepted && cell->Win32Error == ERROR_SUCCESS) {
                cell->Win32Error = ERROR_GEN_FAILURE;
            }
        }
    } else {
        if (cell->Win32Error == ERROR_SUCCESS) {
            cell->Win32Error = ERROR_GEN_FAILURE;
        }
    }

done:
    if (registered != NULL && !WinUsb_UnregisterIsochBuffer(registered)) {
        DWORD unregError = GetLastError();
        printf("[!] UnregisterIsochBuffer failed after alt %u endpoint 0x%02X: Win32 %lu.\n",
               (unsigned)cell->Alt, (unsigned)cell->Endpoint, unregError);
        if (cell->Accepted) {
            cell->FailedAt = "UNREGISTER_ISOCH_BUFFER";
            cell->Win32Error = unregError != ERROR_SUCCESS ? unregError : ERROR_GEN_FAILURE;
            cell->Accepted = FALSE;
        }
    }
    CloseHandle(ov.hEvent);
    free(packets);
}

static BOOL VendorCommand(WINUSB_INTERFACE_HANDLE iface, UCHAR request)
{
    WINUSB_SETUP_PACKET setup;
    ULONG transferred = 0;
    ZeroMemory(&setup, sizeof(setup));
    setup.RequestType = 0x40;
    setup.Request = request;
    return WinUsb_ControlTransfer(iface, setup, NULL, 0, &transferred, NULL);
}

static BOOL RunCancellationRecovery(HANDLE device, WINUSB_INTERFACE_HANDLE control,
                                    WINUSB_INTERFACE_HANDLE isoch, UCHAR *buffer,
                                    ULONG acceptedCount,
                                    CANCEL_PROBE_VERDICT *outVerdict,
                                    DWORD *outError)
{
    WINUSB_ISOCH_BUFFER_HANDLE registered = NULL;
    USBD_ISO_PACKET_DESCRIPTOR packets[4];
    OVERLAPPED ov;
    DWORD bytes = 0;
    DWORD error = ERROR_SUCCESS;
    BOOL pending = FALSE;
    BOOL cancelled = FALSE;
    CELL recovery;

    *outVerdict = CANCEL_PROBE_NOT_RUN;
    *outError = ERROR_SUCCESS;

    if (!WinUsb_SetCurrentAlternateSetting(isoch, 1) || !QueryExpectedPipes(isoch, 1, TRUE)) {
        *outError = GetLastError();
        printf("[-] Cannot select alt 1 for cancellation probe.\n");
        *outVerdict = CANCEL_PROBE_FAILED;
        return FALSE;
    }
    if (!VendorCommand(control, HOLD_NEXT)) {
        *outError = GetLastError();
        printf("[-] HOLD_NEXT failed: Win32 %lu.\n", *outError);
        *outVerdict = CANCEL_PROBE_FAILED;
        return FALSE;
    }
    ZeroMemory(&ov, sizeof(ov));
    ZeroMemory(packets, sizeof(packets));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ov.hEvent == NULL) {
        *outError = GetLastError();
        *outVerdict = CANCEL_PROBE_FAILED;
        goto cleanup;
    }
    if (!WinUsb_RegisterIsochBuffer(isoch, EP_ISOCH_IN, buffer, 36, &registered)) {
        *outError = GetLastError();
        if (acceptedCount == 0) {
            *outVerdict = CANCEL_PROBE_INCONCLUSIVE;
            printf("[*] Cancellation probe INCONCLUSIVE (Win32 %lu): RegisterIsochBuffer failed; cancellation cannot be evaluated until at least one transfer completes.\n", *outError);
        } else {
            *outVerdict = CANCEL_PROBE_FAILED;
            printf("[-] Cancellation probe FAILED: RegisterIsochBuffer failed: Win32 %lu.\n", *outError);
        }
        goto cleanup;
    }

    if (!WinUsb_ReadIsochPipeAsap(registered, 0, 36, FALSE, ARRAYSIZE(packets), packets, &ov)) {
        pending = GetLastError() == ERROR_IO_PENDING;
    }
    if (!pending) {
        *outError = GetLastError();
        if (acceptedCount == 0) {
            *outVerdict = CANCEL_PROBE_INCONCLUSIVE;
            printf("[*] Cancellation probe INCONCLUSIVE (Win32 %lu): held read did not enter pending state; cancellation cannot be evaluated until at least one transfer completes.\n", *outError);
        } else {
            *outVerdict = CANCEL_PROBE_FAILED;
            printf("[-] Cancellation probe FAILED: held read did not enter pending state: Win32 %lu.\n", *outError);
        }
        goto cleanup;
    }
    if (!CancelIoEx(device, &ov) && GetLastError() != ERROR_NOT_FOUND) {
        printf("[-] CancelIoEx failed: Win32 %lu; requesting driver release.\n", GetLastError());
        (void)VendorCommand(control, RELEASE_HELD);
    }
    if (WaitForSingleObject(ov.hEvent, IO_TIMEOUT_MS) != WAIT_OBJECT_0) {
        printf("[-] Cancelled request did not complete within %u ms; requesting release.\n", IO_TIMEOUT_MS);
        (void)VendorCommand(control, RELEASE_HELD);
        if (WaitForSingleObject(ov.hEvent, IO_TIMEOUT_MS) != WAIT_OBJECT_0) {
            fprintf(stderr, "[-] FATAL: held I/O could not be cancelled or released; terminating without freeing live buffers.\n");
            fflush(NULL);
            ExitProcess(EXIT_ERROR);
        }
    }
    if (!WinUsb_GetOverlappedResult(isoch, &ov, &bytes, TRUE)) {
        error = GetLastError();
        cancelled = error == ERROR_OPERATION_ABORTED;
        if (!cancelled) {
            *outError = error;
            if (acceptedCount == 0) {
                *outVerdict = CANCEL_PROBE_INCONCLUSIVE;
                printf("[*] Cancellation probe INCONCLUSIVE (Win32 %lu): cancellation cannot be evaluated until at least one transfer completes.\n", error);
            } else {
                *outVerdict = CANCEL_PROBE_FAILED;
                printf("[-] Cancellation probe FAILED: cancel completion was Win32 %lu, expected ERROR_OPERATION_ABORTED.\n", error);
            }
        }
    } else {
        if (acceptedCount == 0) {
            *outVerdict = CANCEL_PROBE_INCONCLUSIVE;
            *outError = ERROR_SUCCESS;
            printf("[*] Cancellation probe INCONCLUSIVE: cancelled request reported success; cancellation cannot be evaluated until at least one transfer completes.\n");
        } else {
            *outVerdict = CANCEL_PROBE_FAILED;
            *outError = ERROR_INVALID_STATE;
            printf("[-] Cancellation probe FAILED: cancelled request reported success.\n");
        }
    }

cleanup:
    if (!pending) (void)VendorCommand(control, RELEASE_HELD);
    if (registered != NULL) (void)WinUsb_UnregisterIsochBuffer(registered);
    if (ov.hEvent != NULL) CloseHandle(ov.hEvent);
    if (!cancelled) return FALSE;

    ZeroMemory(&recovery, sizeof(recovery));
    recovery.Alt = 1;
    recovery.Endpoint = EP_ISOCH_OUT;
    recovery.Packets = 4;
    recovery.PacketSize = 9;
    recovery.TotalBytes = 36;
    recovery.Category = "post-cancel";
    recovery.FailedAt = "NONE";
    recovery.DeviceSpeed = "UNKNOWN";
    RunCell(device, isoch, &recovery, buffer, DMA_BUFFER_SIZE);
    if (!recovery.Accepted) {
        *outError = recovery.Win32Error;
        if (acceptedCount == 0) {
            *outVerdict = CANCEL_PROBE_INCONCLUSIVE;
            printf("[*] Cancellation probe INCONCLUSIVE (Win32 %lu): post-cancel transfer failed at %s; cancellation cannot be evaluated until at least one transfer completes.\n",
                   recovery.Win32Error, recovery.FailedAt);
        } else {
            *outVerdict = CANCEL_PROBE_FAILED;
            printf("[-] Cancellation probe FAILED: post-cancel transfer failed at %s: Win32 %lu.\n",
                   recovery.FailedAt, recovery.Win32Error);
        }
        return FALSE;
    }
    printf("[+] Cancellation drained and subsequent transfer completed.\n");
    *outVerdict = CANCEL_PROBE_PASSED;
    return TRUE;
}

static void Usage(void)
{
    printf("Usage: isotest.exe [--plan] [--csv <path>]\n");
    printf("  --plan       enumerate the exact transfer cells without opening a device\n");
    printf("  --csv path   write the plan or measured results to path\n");
    printf("Exit: 0 completed, 1 usage/instrument failure, 2 live device absent.\n");
}

int wmain(int argc, wchar_t **argv)
{
    BOOL plan = FALSE;
    const WCHAR *csvPath = NULL;
    WCHAR devicePath[MAX_PATH];
    DWORD findError = ERROR_SUCCESS;
    HANDLE device = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE control = NULL;
    WINUSB_INTERFACE_HANDLE isoch = NULL;
    UCHAR *buffer = NULL;
    BOOL infrastructureOk = TRUE;
    BOOL frameNumberAvailable = FALSE;
    const char *failingStage = NULL;
    CANCEL_PROBE_VERDICT cancelVerdict = CANCEL_PROBE_NOT_RUN;
    DWORD cancelError = ERROR_SUCCESS;
    ULONG acceptedCount = 0;
    ULONG i;
    UCHAR currentAlt = 0xFF;
    int result = EXIT_ERROR;
    const char *deviceSpeedStr = "UNKNOWN";

    printf("[*] Harness build: " HARNESS_BUILD_STAMP "\n");
    for (i = 1; i < (ULONG)argc; ++i) {
        if (_wcsicmp(argv[i], L"--plan") == 0) {
            plan = TRUE;
        } else if (_wcsicmp(argv[i], L"--csv") == 0 && i + 1 < (ULONG)argc) {
            csvPath = argv[++i];
        } else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0) {
            Usage();
            return EXIT_OK;
        } else {
            fwprintf(stderr, L"[-] Unknown or incomplete option: %ls\n", argv[i]);
            Usage();
            return EXIT_ERROR;
        }
    }

    PopulatePlan();
    if (plan) {
        printf("[*] Plan enumerates %lu exact WinUSB API calls; OUT has no packet-count argument.\n",
               g_CellCount);
        PrintCells(stdout, TRUE);
        return csvPath == NULL || WriteCsv(csvPath, TRUE) ? EXIT_OK : EXIT_ERROR;
    }

    printf("[*] Searching for DeckBtIsoTest WinUSB interface.\n");
    if (!FindDevicePath(devicePath, ARRAYSIZE(devicePath), &findError)) {
        if (findError == ERROR_NO_MORE_ITEMS || findError == ERROR_NOT_FOUND) {
            printf("[-] DEVICE NOT FOUND: install/start root\\DeckBtIsoTest, then retry (exit 2).\n");
            return EXIT_DEVICE_NOT_FOUND;
        }
        printf("[-] Device enumeration failed: Win32 %lu.\n", findError);
        return EXIT_ERROR;
    }

    device = CreateFileW(devicePath, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (device == INVALID_HANDLE_VALUE) {
        printf("[-] CreateFileW failed: Win32 %lu.\n", GetLastError());
        failingStage = "interface open";
        printf("[-] Measurement incomplete: interface open failed.\n");
        goto cleanup;
    }
    if (!WinUsb_Initialize(device, &control)) {
        printf("[-] WinUsb_Initialize failed: Win32 %lu.\n", GetLastError());
        failingStage = "interface open";
        printf("[-] Measurement incomplete: interface open failed.\n");
        goto cleanup;
    }
    if (!WinUsb_GetAssociatedInterface(control, 0, &isoch)) {
        printf("[-] Interface 1 is required; GetAssociatedInterface failed: Win32 %lu.\n", GetLastError());
        failingStage = "interface open";
        printf("[-] Measurement incomplete: interface open failed.\n");
        goto cleanup;
    }
    {
        UCHAR speed = 0;
        ULONG speedLength = sizeof(speed);
        if (WinUsb_QueryDeviceInformation(control, DEVICE_SPEED, &speedLength, &speed) ||
            WinUsb_QueryDeviceInformation(isoch, DEVICE_SPEED, &speedLength, &speed)) {
            if (speed == HighSpeed) {
                deviceSpeedStr = "HIGH";
            } else if (speed == FullSpeed) {
                deviceSpeedStr = "FULL";
            } else {
                deviceSpeedStr = "UNKNOWN";
            }
            printf("[+] QueryDeviceInformation(DEVICE_SPEED): %s (0x%02X).\n",
                   deviceSpeedStr, (unsigned)speed);
        } else {
            printf("[*] QueryDeviceInformation(DEVICE_SPEED) failed: Win32 %lu; stamping UNKNOWN.\n",
                   GetLastError());
            deviceSpeedStr = "UNKNOWN";
        }
    }

    {
        ULONG frame = 0;
        LARGE_INTEGER timestamp;
        if (WinUsb_GetCurrentFrameNumber(isoch, &frame, &timestamp)) {
            frameNumberAvailable = TRUE;
            printf("[+] GetCurrentFrameNumber supported: frame %lu.\n", frame);
        } else {
            printf("[*] GetCurrentFrameNumber unavailable: Win32 %lu; ASAP cells will still be measured.\n", GetLastError());
        }
    }

    if (!WinUsb_SetCurrentAlternateSetting(isoch, 0) || !QueryExpectedPipes(isoch, 0, FALSE)) {
        printf("[-] Alt 0 zero-bandwidth geometry could not be verified.\n");
        infrastructureOk = FALSE;
        if (failingStage == NULL) failingStage = "alternate-setting selection";
    }
    buffer = (UCHAR *)VirtualAlloc(NULL, DMA_BUFFER_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (buffer == NULL) {
        printf("[-] VirtualAlloc failed: Win32 %lu.\n", GetLastError());
        printf("[-] Measurement incomplete: instrument setup failed.\n");
        goto cleanup;
    }

    for (i = 0; i < g_CellCount; ++i) {
        CELL *cell = &g_Cells[i];
        cell->DeviceSpeed = deviceSpeedStr;
        if (cell->Alt != currentAlt) {
            currentAlt = cell->Alt;
            if (!WinUsb_SetCurrentAlternateSetting(isoch, currentAlt) ||
                !QueryExpectedPipes(isoch, currentAlt, TRUE)) {
                printf("[-] Alt %u activation/geometry failed; its cells are not attempted.\n",
                       (unsigned)currentAlt);
                infrastructureOk = FALSE;
                if (failingStage == NULL) failingStage = "alternate-setting selection";
                while (i < g_CellCount && g_Cells[i].Alt == currentAlt) {
                    g_Cells[i].DeviceSpeed = deviceSpeedStr;
                    g_Cells[i].FailedAt = "HARNESS_PRECHECK";
                    g_Cells[i].Win32Error = ERROR_INVALID_DATA;
                    g_Cells[i].Accepted = FALSE;
                    ++i;
                }
                --i;
                continue;
            }
        }
        RunCell(device, isoch, cell, buffer, DMA_BUFFER_SIZE);
        if (cell->Win32Error == ERROR_TIMEOUT) {
            infrastructureOk = FALSE;
            if (failingStage == NULL) failingStage = "transfer execution";
        }
    }
    printf("[*] Recorded %lu attempted API calls; OUT packetization is controlled by WinUSB.\n",
           g_CellCount);

    for (i = 0; i < g_CellCount; ++i) {
        if (g_Cells[i].Accepted) acceptedCount++;
    }

    if (!RunCancellationRecovery(device, control, isoch, buffer, acceptedCount, &cancelVerdict, &cancelError)) {
        infrastructureOk = FALSE;
        if (failingStage == NULL) failingStage = "cancellation probe";
    }
    PrintOutConfirmationLegend(stdout);
    PrintCells(stdout, FALSE);
    if (csvPath != NULL && !WriteCsv(csvPath, FALSE)) {
        infrastructureOk = FALSE;
        if (failingStage == NULL) failingStage = "CSV write";
    }
    PrintMeasurementSummary(stdout, frameNumberAvailable);
    if (infrastructureOk) {
        printf("[+] Measurement completed; ACCEPTED/REJECTED rows are observations, not fabricated pass criteria.\n");
        result = EXIT_OK;
    } else {
        if (failingStage == NULL) {
            failingStage = "instrument setup";
        }
        if (strcmp(failingStage, "cancellation probe") == 0) {
            if (cancelVerdict == CANCEL_PROBE_INCONCLUSIVE) {
                printf("[-] Measurement incomplete: cancellation probe INCONCLUSIVE (Win32 %lu).\n",
                       cancelError);
            } else {
                printf("[-] Measurement incomplete: cancellation probe FAILED (Win32 %lu).\n",
                       cancelError);
            }
        } else {
            printf("[-] Measurement incomplete: %s failed.\n", failingStage);
        }
    }
cleanup:
    if (buffer != NULL) VirtualFree(buffer, 0, MEM_RELEASE);
    if (isoch != NULL) WinUsb_Free(isoch);
    if (control != NULL) WinUsb_Free(control);
    if (device != INVALID_HANDLE_VALUE) CloseHandle(device);
    return result;
}
