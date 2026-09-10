<#
    m1-diag.ps1 - Diagnostic script reading execution status codes and EP0 transfer logs
    recorded in HKLM\SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters.
#>

$ErrorActionPreference = 'Continue'

$steps = @{
    1  = 'DriverEntry completed (WdfDriverCreate)'
    2  = 'UdecxInitializeWdfDeviceInit'
    3  = 'WdfDeviceCreate'
    4  = 'WdfDeviceCreateDeviceInterface(GUID_DEVINTERFACE_USB_HOST_CONTROLLER)'
    5  = 'WdfSpinLockCreate'
    6  = 'WdfIoQueueCreate (event queue)'
    7  = 'WdfIoQueueCreate (ACL IN queue)'
    8  = 'UdecxWdfDeviceAddUsbDeviceEmulation'
    9  = 'EvtDeviceAdd COMPLETED SUCCESSFULLY'
    20 = 'EvtDevicePrepareHardware entered'
    21 = 'DeckBtCreateUsbDevice returned'
    22 = 'UdecxUsbDevicePlugIn (via PrepareHardware) returned'
    30 = 'UdecxUsbDeviceInitAllocate'
    31 = 'UdecxUsbDeviceInitAddDescriptor (device descriptor)'
    32 = 'UdecxUsbDeviceInitAddDescriptor (configuration descriptor)'
    33 = 'UdecxUsbDeviceInitAddStringDescriptorRaw (LANGIDs)'
    34 = 'UdecxUsbDeviceInitAddStringDescriptorRaw (strings)'
    35 = 'UdecxUsbDeviceCreate'
    36 = 'UdecxUsbDevicePlugIn'
    40 = 'EvtUsbDeviceDefaultEndpointAdd (EP0 queue)'
    41 = 'EvtUsbDeviceEndpointAdd (data endpoint queue)'
    42 = 'EvtUsbDeviceReset (Device reset request received)'
}

$known = @{
    0          = 'STATUS_SUCCESS'
    0xC000000D = 'STATUS_INVALID_PARAMETER'
    0xC0000002 = 'STATUS_NOT_IMPLEMENTED'
    0xC00000BB = 'STATUS_NOT_SUPPORTED'
    0xC000009A = 'STATUS_INSUFFICIENT_RESOURCES'
    0xC0000010 = 'STATUS_INVALID_DEVICE_REQUEST'
    0xC0000225 = 'STATUS_NOT_FOUND'
    0xC0000001 = 'STATUS_UNSUCCESSFUL'
    0xC0000184 = 'STATUS_INVALID_DEVICE_STATE'
}

$key = 'HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters'

Write-Host ''
Write-Host '===== DeckBtUsb driver breadcrumbs =====' -ForegroundColor Cyan

if (-not (Test-Path $key)) {
    Write-Host "  Parameters key not present: $key" -ForegroundColor Red
    Write-Host '  The driver has never run, or the package is not installed.' -ForegroundColor Red
} else {
    $p = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
    $step = $p.LastAddDeviceStep
    $st   = $p.LastAddDeviceStatus

    if ($null -eq $step) {
        Write-Host '  No step recorded - the driver image never reached DriverEntry.' -ForegroundColor Red
    } else {
        $name = if ($steps.ContainsKey([int]$step)) { $steps[[int]$step] } else { "(unknown step $step)" }
        $hex  = '0x{0:X8}' -f ([uint32]$st)
        $sn   = if ($known.ContainsKey([uint32]$st)) { $known[[uint32]$st] } else { 'see ntstatus.h' }

        Write-Host "  last step   : $step - $name"
        Write-Host "  last status : $hex  $sn"
        Write-Host ''
        if ([uint32]$st -eq 0) {
            Write-Host "  => That call SUCCEEDED. The failure is in whatever runs next." -ForegroundColor Yellow
        } else {
            Write-Host "  => FAILING CALL: $name" -ForegroundColor Red
        }
    }
}

Write-Host ''
Write-Host '===== EP0 control requests BTHUSB issued =====' -ForegroundColor Cyan
$stdReq = @{
    0='GET_STATUS'; 1='CLEAR_FEATURE'; 3='SET_FEATURE'; 5='SET_ADDRESS'
    6='GET_DESCRIPTOR'; 7='SET_DESCRIPTOR'; 8='GET_CONFIGURATION'; 9='SET_CONFIGURATION'
    10='GET_INTERFACE'; 11='SET_INTERFACE'; 12='SYNCH_FRAME'
}
$descType = @{
    1='DEVICE'; 2='CONFIGURATION'; 3='STRING'; 4='INTERFACE'; 5='ENDPOINT'
    6='DEVICE_QUALIFIER'; 7='OTHER_SPEED_CONFIG'; 8='INTERFACE_POWER'; 15='BOS'
}
$outcome = @{ 1='handled as HCI command'; 2='STALLED'; 3='buffer error'; 4='not a control transfer' }
$hciOps = @{
    0x0C03='HCI_Reset'; 0x0C01='Set_Event_Mask'; 0x0C63='Set_Event_Mask_Page_2'
    0x0C6D='Write_LE_Host_Support'; 0x1001='Read_Local_Version'; 0x1002='Read_Local_Supported_Commands'
    0x1003='Read_Local_Features'; 0x1004='Read_Local_Extended_Features'; 0x1005='Read_Buffer_Size'
    0x1009='Read_BD_ADDR'; 0x100B='Read_Local_Supported_Codecs'; 0x0C14='Read_Local_Name'
    0x0C13='Write_Local_Name'; 0x0C17='Read_Page_Timeout'; 0x0C19='Read_Scan_Enable'
    0x0C23='Read_Class_Of_Device'; 0x0C25='Read_Voice_Setting'; 0x0C33='Host_Buffer_Size'
    0x2002='LE_Read_Buffer_Size'; 0x2003='LE_Read_Local_Features'; 0x201C='LE_Read_Supported_States'
    0x2007='LE_Read_Adv_Tx_Power'; 0x200F='LE_Read_Accept_List_Size'
    0x202A='LE_Read_Resolving_List_Size'; 0x0C56='Write_Simple_Pairing_Mode'
    0x0C52='Write_Extended_Inquiry_Response'; 0x0C1A='Write_Scan_Enable'
    0x0C58='Read_Inquiry_Rsp_Tx_Power'; 0x0C18='Write_Page_Timeout'
    0x0C1C='Write_Page_Scan_Activity'; 0x0C1E='Write_Inquiry_Scan_Activity'
    0x0C43='Write_Inquiry_Scan_Type'; 0x0C45='Write_Inquiry_Mode'
    0x0C47='Write_Page_Scan_Type'; 0x0C24='Write_Class_Of_Device'
    0x0C20='Write_Authentication_Enable'; 0x0C0D='Read_Stored_Link_Key'
    0x0C11='Write_Stored_Link_Key'; 0x0C12='Delete_Stored_Link_Key'
    0x2001='LE_Set_Event_Mask'; 0x202F='LE_Read_Max_Data_Length'
    0x2023='LE_Read_Suggested_Default_Data_Length'; 0x2024='LE_Write_Suggested_Default_Data_Length'
}

if (Test-Path $key) {
    $p2 = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
    $blob = $p2.ControlLog
    $n    = $p2.ControlCount
    if ($null -eq $blob) {
        Write-Host '  no control requests recorded yet'
    } else {
        # The ring is written at index (count % slots), so slot order is not chronological and
        # the slot count must come from the blob, not from a hard-coded guess.
        $capacity = [int]($blob.Length / 12)
        $shown    = [Math]::Min([int]$n, $capacity)
        $first    = [int]$n - $shown
        Write-Host "  total control transfers seen: $n   (showing the last $shown, oldest first)"
        for ($seq = $first; $seq -lt [int]$n; $seq++) {
            $i = $seq
            $o = ($seq % $capacity) * 12
            $bm = $blob[$o]; $br = $blob[$o+1]
            $wv = [int]$blob[$o+2] + ([int]$blob[$o+3] -shl 8)
            $wi = [int]$blob[$o+4] + ([int]$blob[$o+5] -shl 8)
            $wl = [int]$blob[$o+6] + ([int]$blob[$o+7] -shl 8)
            $oc = $blob[$o+8]
            # Skip never-written slots rather than decoding zeroes as a bogus GET_STATUS.
            if ($bm -eq 0 -and $br -eq 0 -and $wl -eq 0 -and $oc -eq 0 -and
                [int]$blob[$o+10] -eq 0 -and [int]$blob[$o+11] -eq 0) { continue }

            $dir  = if ($bm -band 0x80) { 'IN ' } else { 'OUT' }
            $type = switch (($bm -shr 5) -band 3) { 0 {'standard'} 1 {'class'} 2 {'vendor'} default {'reserved'} }
            $name = if ($type -eq 'standard' -and $stdReq.ContainsKey([int]$br)) { $stdReq[[int]$br] } else { "req 0x{0:X2}" -f $br }
            $extra = ''
            if ($type -eq 'standard' -and $br -eq 6) {
                $dt = ($wv -shr 8) -band 0xFF
                $di = $wv -band 0xFF
                $dn = if ($descType.ContainsKey([int]$dt)) { $descType[[int]$dt] } else { "type $dt" }
                $extra = "  descriptor=$dn index=$di"
            }
            $ocn = if ($outcome.ContainsKey([int]$oc)) { $outcome[[int]$oc] } else { "outcome $oc" }
            $evtLen = $blob[$o+9]
            $opc    = [int]$blob[$o+10] + ([int]$blob[$o+11] -shl 8)
            if ($opc -ne 0) {
                $opn = if ($hciOps.ContainsKey([int]$opc)) { $hciOps[[int]$opc] } else { ('opcode 0x{0:X4}' -f $opc) }
                $flag = if ($evtLen -eq 0) { '  <== NO EVENT PRODUCED (BTHUSB will time out)' }
                        elseif ($evtLen -le 6) { '  (status-only reply)' } else { '' }
                Write-Host ("  [{0,2}] HCI {1,-32} reply={2,3} bytes{3}" -f $i, $opn, $evtLen, $flag) `
                           -ForegroundColor $(if ($evtLen -eq 0) { 'Red' } else { 'Gray' })
            } else {
                Write-Host ("  [{0,2}] bm=0x{1:X2} {2} {3,-8} {4,-18} wValue=0x{5:X4} wIndex=0x{6:X4} len={7,-4}{8}  -> {9}" -f `
                            $i, $bm, $dir, $type, $name, $wv, $wi, $wl, $extra, $ocn)
            }
        }
        Write-Host ''
        $unanswered = 0
        for ($seq = $first; $seq -lt [int]$n; $seq++) {
            $o = ($seq % $capacity) * 12
            if ([int]$blob[$o+9] -eq 0 -and ([int]$blob[$o+10] -ne 0 -or [int]$blob[$o+11] -ne 0)) { $unanswered++ }
        }
        if ($unanswered -gt 0) {
            Write-Host "  $unanswered command(s) produced NO event - those are the timeouts." -ForegroundColor Red
        }
        Write-Host ''
        Write-Host '  NOTE: Standard requests answered directly by UdeCx do not appear here.'
        Write-Host '        Entries below reached the driver endpoint 0 handler.'
    }
}

Write-Host ''
Write-Host '===== devnode state =====' -ForegroundColor Cyan
$dev = Get-PnpDevice -InstanceId 'ROOT\DEVGEN\DECKBTUSB' -ErrorAction SilentlyContinue
if ($dev) {
    $svc = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
    $pc  = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue).Data
    $ps  = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_ProblemStatus' -ErrorAction SilentlyContinue).Data
    Write-Host "  $($dev.InstanceId)"
    Write-Host "  status=$($dev.Status) service='$svc' problem=$pc ntstatus=$ps"
} else {
    Write-Host '  ROOT\DEVGEN\DECKBTUSB not present'
}

$child = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\VID_0CF3&PID_6390*' })
Write-Host ''
if ($child.Count -gt 0) {
    foreach ($c in $child) {
        $svc = (Get-PnpDeviceProperty -InstanceId $c.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
        Write-Host "  EMULATED RADIO: $($c.Status) service='$svc' :: $($c.InstanceId)" -ForegroundColor Green
    }
} else {
    Write-Host '  emulated radio USB\VID_0CF3&PID_6390: not present'
}

Write-Host ''
Write-Host '===== bus-interface handshake seen by DeckBtFlt =====' -ForegroundColor Cyan
# Entry layout, 32 bytes: Guid[16] Size(2) Version(2) Status(4) Minor(4) Sequence(4)
$fltKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtFlt\Parameters'
$ifaceNames = @{
    'b1a96a13-3de0-4574-9b01-c08feab318d6' = 'USB_BUS_INTERFACE_USBDI      <-- the one BTHUSB needs'
    '893b6a96-0b7f-4d4d-bdb4-bbd4ceebb31c' = 'USB_BUS_INTERFACE_USBC_CONFIGURATION'
    'b1a96a13-3de0-4574-9b01-c08feab318d7' = 'USB_BUS_INTERFACE_HUB'
    '2ea9d0d6-4ad0-4e88-9dbd-0cbb0f8a80a1' = 'GUID_PNP_LOCATION_INTERFACE'
    'b38290e5-3cd0-4f9d-9937-f5fe2b44d47a' = 'GUID_D3COLD_SUPPORT_INTERFACE (power mgmt, benign)'
    '649fdf26-3bc0-4813-ad24-7e0c1eda3fa3' = 'GUID_DEVICE_RESET_INTERFACE_STANDARD  <-- recovery path'
    '2aeb0243-6a6e-486b-82fc-d815f6b97006' = 'GUID_REENUMERATE_SELF_INTERFACE_STANDARD <-- recovery path'
    '14ea9746-6cb7-4a5b-8b3d-1b3bdb6d0c9a' = 'GUID_BUS_INTERFACE_STANDARD'
}
$statusNames = @{
    0          = 'STATUS_SUCCESS'
    0xC00000BB = 'STATUS_NOT_SUPPORTED   <-- the stack REFUSED this interface'
    0xC0000002 = 'STATUS_NOT_IMPLEMENTED <-- the stack REFUSED this interface'
    0xC000000D = 'STATUS_INVALID_PARAMETER'
    0xC0000225 = 'STATUS_NOT_FOUND'
    0xC0000010 = 'STATUS_INVALID_DEVICE_REQUEST'
    0xC000009A = 'STATUS_INSUFFICIENT_RESOURCES'
}

if (-not (Test-Path $fltKey)) {
    Write-Host '  DeckBtFlt is not installed yet (no Parameters key).'
} else {
    $fp = Get-ItemProperty -Path $fltKey -ErrorAction SilentlyContinue
    $blob = $fp.QueryInterfaceLog
    Write-Host ("  PnP IRPs seen: {0}   QUERY_INTERFACE count: {1}" -f $fp.PnpIrpCount, $fp.QueryInterfaceCount)
    if ($null -eq $blob) {
        Write-Host '  no QUERY_INTERFACE recorded - the filter loaded but saw none, or never loaded' -ForegroundColor Yellow
    } else {
        $n = [Math]::Min([int]$fp.QueryInterfaceCount, 32)
        for ($i = 0; $i -lt $n; $i++) {
            $o = $i * 32
            $g = [Guid]::new([byte[]]$blob[$o..($o+15)])
            $size = [int]$blob[$o+16] + ([int]$blob[$o+17] -shl 8)
            $ver  = [int]$blob[$o+18] + ([int]$blob[$o+19] -shl 8)
            $st   = [uint32]([int]$blob[$o+20] + ([int]$blob[$o+21] -shl 8) + ([int]$blob[$o+22] -shl 16) + ([uint32][int]$blob[$o+23] -shl 24))
            $key2 = $g.ToString()
            $nm = if ($ifaceNames.ContainsKey($key2)) { $ifaceNames[$key2] } else { $key2 }
            $sn = if ($statusNames.ContainsKey($st)) { $statusNames[$st] } else { ('0x{0:X8}' -f $st) }
            $colour = if ($st -eq 0) { 'Gray' } else { 'Red' }
            Write-Host ("  [{0,2}] size=0x{1:X2} ver={2}  {3}" -f $i, $size, $ver, $nm) -ForegroundColor $colour
            Write-Host ("       -> {0}" -f $sn) -ForegroundColor $colour
        }
    }
}

Write-Host ''
Write-Host '===== driver image =====' -ForegroundColor Cyan
$sys = 'C:\Windows\System32\drivers\deckbtusb.sys'
if (Test-Path $sys) {
    $i = Get-Item $sys
    Write-Host "  installed: $($i.Length) bytes, $($i.LastWriteTime)"
} else {
    Write-Host "  $sys not present (driver store copy only)"
}
Write-Host ''
