param([string]$Address = '192.168.0.223', [int]$Port = 5000)

$ErrorActionPreference = 'Stop'
foreach ($size in @(5, 256, 2048)) {
    $client = [Net.Sockets.TcpClient]::new()
    $client.ReceiveTimeout = 3000
    $client.SendTimeout = 3000
    try {
        $client.Connect($Address, $Port)
        $stream = $client.GetStream()
        $sent = [byte[]]::new($size)
        for ($i = 0; $i -lt $size; $i++) { $sent[$i] = [byte](($i * 31 + 7) % 256) }
        $stream.Write($sent, 0, $sent.Length)
        $received = [byte[]]::new($size)
        $offset = 0
        while ($offset -lt $size) {
            $count = $stream.Read($received, $offset, $size - $offset)
            if ($count -eq 0) { throw "Connection closed after $offset of $size bytes" }
            $offset += $count
        }
        if ([Convert]::ToBase64String($sent) -ne [Convert]::ToBase64String($received)) {
            throw "TCP echo data mismatch ($size bytes)"
        }
        Write-Host "PASS: $size bytes echoed correctly"
    }
    finally { $client.Dispose() }
}
