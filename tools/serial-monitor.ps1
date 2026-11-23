param(
    [string]$Port = 'COM3',
    [int]$BaudRate = 115200,
    [int]$DurationSeconds = 0,
    [switch]$Hex
)

$serial = [System.IO.Ports.SerialPort]::new($Port, $BaudRate, 'None', 8, 'One')
$serial.Handshake = [System.IO.Ports.Handshake]::None
$serial.ReadTimeout = 250
$serial.NewLine = "`n"
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$timer = [System.Diagnostics.Stopwatch]::StartNew()

try {
    $serial.Open()
    Write-Host "Listening on $Port at $BaudRate baud (8-N-1). Press Ctrl+C to stop."
    while ($DurationSeconds -le 0 -or $timer.Elapsed.TotalSeconds -lt $DurationSeconds) {
        try {
            if ($Hex) {
                $value = $serial.ReadByte()
                Write-Host ('[{0}] {1:X2}' -f (Get-Date -Format 'HH:mm:ss.fff'), $value)
            }
            else {
                $line = $serial.ReadLine().TrimEnd("`r")
                Write-Host "[$(Get-Date -Format 'HH:mm:ss.fff')] $line"
            }
        }
        catch [System.TimeoutException] { }
    }
}
finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
}
