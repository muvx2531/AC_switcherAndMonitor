param(
  [ValidateSet('GET','SET','BACKUP','BATV','CLRSAFE')][string]$Command = 'GET',
  [ValidateRange(0,15)][int]$Value = 0,
  [string]$Port = 'COM3',
  [ValidateRange(0,255)][int]$Sequence = 1,
  [int]$TimeoutMs = 3000
)
$ErrorActionPreference = 'Stop'
$seq = '{0:X2}' -f $Sequence
$body = if ($Command -eq 'GET') { "$seq,GET" } elseif ($Command -eq 'BACKUP') { "$seq,BACKUP" } elseif ($Command -eq 'BATV') { "$seq,BATV" } elseif ($Command -eq 'CLRSAFE') { "$seq,CLRSAFE" } else { "$seq,SET,{0:X}" -f $Value }
function Checksum([string]$value) {
  $sum = 0
  foreach ($byte in [Text.Encoding]::ASCII.GetBytes($value)) { $sum = ($sum + $byte) -band 255 }
  return ('{0:X2}' -f $sum)
}
$frame = '@' + $body + '*' + (Checksum $body) + "`r`n"
$serial = [IO.Ports.SerialPort]::new($Port, 115200, 'None', 8, 'One')
$serial.ReadTimeout = 250
try {
  $serial.Open()
  $serial.DiscardInBuffer()
  $serial.Write($frame)
  $timer = [Diagnostics.Stopwatch]::StartNew()
  while ($timer.ElapsedMilliseconds -lt $TimeoutMs) {
    try { $line = $serial.ReadLine().TrimEnd("`r") } catch [TimeoutException] { continue }
    if ($line -notmatch '^@([0-9A-F]{2},[^*]+)\*([0-9A-F]{2})$') { continue }
    $payload = $Matches[1]; $received = $Matches[2]
    if ((Checksum $payload) -ne $received) { throw 'Response checksum mismatch' }
    if (-not $payload.StartsWith("$seq,")) { continue }
    Write-Output $line
    if ($payload -match '^[0-9A-F]{2},ERR,') { throw "Device rejected command: $payload" }
    return
  }
  throw "Timed out waiting for response on $Port"
}
finally { $serial.Dispose() }
