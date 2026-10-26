# tools/_common.ps1 - shared helpers, dot-sourced by every tools/*.ps1.
# Requires PowerShell 7+ (Invoke-RestMethod -SkipCertificateCheck / -Form).
#Requires -Version 7

Set-StrictMode -Version Latest
$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$script:DpSession = $null   # Device Portal cookie jar (CSRF); primed by Initialize-DpSession

# Package identity. Must match host/Package.appxmanifest <Identity Name=...>.
$script:PackageName = 'xbox-rl.host'
$script:AppId       = 'App'

function Import-DotEnv {
    # Reads KEY=VALUE lines from <repo>/.env into a hashtable (does NOT touch the
    # process environment, so secrets don't leak into child processes).
    # XRL_ENV_FILE overrides the location (used by tools/test/test-tools.ps1).
    param([string]$Path = $(if ($env:XRL_ENV_FILE) { $env:XRL_ENV_FILE } else { Join-Path $script:RepoRoot '.env' }))
    $h = @{}
    if (-not (Test-Path $Path)) {
        throw ".env not found at $Path - copy .env.example to .env and fill in XBOX_IP / XBOX_USER / XBOX_PASS."
    }
    foreach ($line in Get-Content $Path) {
        $l = $line.Trim()
        if ($l -eq '' -or $l.StartsWith('#')) { continue }
        $i = $l.IndexOf('=')
        if ($i -lt 1) { continue }
        $k = $l.Substring(0, $i).Trim()
        $v = $l.Substring($i + 1).Trim()
        if (($v.StartsWith('"') -and $v.EndsWith('"')) -or ($v.StartsWith("'") -and $v.EndsWith("'"))) {
            $v = $v.Substring(1, $v.Length - 2)
        }
        $h[$k] = $v
    }
    return $h
}

function Get-XboxConfig {
    $e = Import-DotEnv
    foreach ($k in 'XBOX_IP', 'XBOX_USER', 'XBOX_PASS') {
        if (-not $e.ContainsKey($k) -or [string]::IsNullOrWhiteSpace($e[$k])) {
            throw "$k is missing/empty in .env"
        }
    }
    $sec = ConvertTo-SecureString $e['XBOX_PASS'] -AsPlainText -Force
    [pscustomobject]@{
        Ip      = $e['XBOX_IP']
        User    = $e['XBOX_USER']
        Cred    = [pscredential]::new($e['XBOX_USER'], $sec)
        # XBOX_URL is a test hook (point the tools at a mock Device Portal); normally unset.
        Base    = $(if ($e.ContainsKey('XBOX_URL') -and $e['XBOX_URL']) { $e['XBOX_URL'].TrimEnd('/') } else { "https://$($e['XBOX_IP']):11443" })
        GameSrc = $(if ($e.ContainsKey('GAME_SRC')) { $e['GAME_SRC'] } else { $null })
    }
}

function Initialize-DpSession {
    # Windows Device Portal uses CSRF double-submit protection: a GET first yields
    # a 'CSRF-Token' cookie, which must be echoed back as the 'X-CSRF-Token' header
    # on every POST/DELETE. We also reuse the cookie jar across calls via a
    # module-scoped WebSession. Priming with /api/os/info (already known reachable).
    param([Parameter(Mandatory)] $Xbox, [switch]$Force)
    if ($script:DpSession -and -not $Force) { return }
    $p = @{
        Uri                  = "$($Xbox.Base)/api/os/info"
        Credential           = $Xbox.Cred
        Authentication       = 'Basic'
        SkipCertificateCheck = $true
        SessionVariable      = 'sess'
        TimeoutSec           = 60
    }
    if ($Xbox.Base.StartsWith('http://')) { $p['AllowUnencryptedAuthentication'] = $true }
    Invoke-WebRequest @p | Out-Null
    $script:DpSession = $sess
}

function Get-DpCsrf {
    if (-not $script:DpSession) { return $null }
    $c = $script:DpSession.Cookies.GetAllCookies() | Where-Object { $_.Name -eq 'CSRF-Token' } | Select-Object -First 1
    if ($c) { return $c.Value }
    return $null
}

function Invoke-Dp {
    # Device Portal REST call. Self-signed console cert => skip cert check.
    # Basic auth + CSRF token (see Initialize-DpSession). Throws with the HTTP status +
    # body (and $err.Data['Status']) so callers can show the real reason.
    param(
        [Parameter(Mandatory)] $Xbox,
        [Parameter(Mandatory)] [string]$Path,
        [string]$Method = 'GET',
        $Form = $null,
        [string]$OutFile = $null,
        [int]$TimeoutSec = 120,
        [switch]$Raw
    )
    Initialize-DpSession $Xbox
    $p = @{
        Uri                  = "$($Xbox.Base)$Path"
        Method               = $Method
        Credential           = $Xbox.Cred
        Authentication       = 'Basic'
        SkipCertificateCheck = $true
        TimeoutSec           = $TimeoutSec
        WebSession           = $script:DpSession
    }
    if ($Xbox.Base.StartsWith('http://')) { $p['AllowUnencryptedAuthentication'] = $true }   # mock/test only
    if ($Method -ne 'GET') {
        $csrf = Get-DpCsrf
        if ($csrf) { $p['Headers'] = @{ 'X-CSRF-Token' = $csrf } }
    }
    if ($Form)    { $p['Form'] = $Form }
    if ($OutFile) { $p['OutFile'] = $OutFile }
    try {
        if ($Raw) { return Invoke-WebRequest @p }
        return Invoke-RestMethod @p
    } catch {
        $status = $null; $body = ''
        if ($_.Exception.PSObject.Properties['Response'] -and $_.Exception.Response) {
            $status = [int]$_.Exception.Response.StatusCode
        }
        if ($_.ErrorDetails) { $body = $_.ErrorDetails.Message }
        $err = [System.Exception]::new("Device Portal $Method $Path failed: HTTP $status $body ($($_.Exception.Message))")
        $err.Data['Status'] = $status
        throw $err
    }
}

function Invoke-DpMultipart {
    # Manual multipart/form-data upload. PowerShell's -Form does not shape the body
    # the way WDP's /packagemanager/package parser wants (it reports "Missing .msix"),
    # so we build each part by hand: name=filename, explicit octet-stream, CRLFs. Body
    # is assembled in memory (fine for the small app package; NOT for the 20 GB game,
    # which goes through the chunked filesystem API in Send-DpFile instead).
    param(
        [Parameter(Mandatory)] $Xbox,
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string[]]$Files,
        [int]$TimeoutSec = 900
    )
    Initialize-DpSession $Xbox
    $boundary = "----xrl$([guid]::NewGuid().ToString('N'))"
    $enc = [System.Text.Encoding]::ASCII
    $ms  = [System.IO.MemoryStream]::new()
    $put = { param($s) $b = $enc.GetBytes($s); $ms.Write($b, 0, $b.Length) }
    foreach ($f in $Files) {
        $name = [IO.Path]::GetFileName($f)
        & $put "--$boundary`r`n"
        & $put "Content-Disposition: form-data; name=`"$name`"; filename=`"$name`"`r`n"
        & $put "Content-Type: application/octet-stream`r`n`r`n"
        $bytes = [IO.File]::ReadAllBytes($f)
        $ms.Write($bytes, 0, $bytes.Length)
        & $put "`r`n"
    }
    & $put "--$boundary--`r`n"
    $body = $ms.ToArray(); $ms.Dispose()

    $headers = @{}
    $csrf = Get-DpCsrf; if ($csrf) { $headers['X-CSRF-Token'] = $csrf }
    $p = @{
        Uri                  = "$($Xbox.Base)$Path"
        Method               = 'POST'
        Credential           = $Xbox.Cred
        Authentication       = 'Basic'
        SkipCertificateCheck = $true
        WebSession           = $script:DpSession
        TimeoutSec           = $TimeoutSec
        Body                 = $body
        ContentType          = "multipart/form-data; boundary=$boundary"
        Headers              = $headers
    }
    if ($Xbox.Base.StartsWith('http://')) { $p['AllowUnencryptedAuthentication'] = $true }
    try {
        return Invoke-RestMethod @p
    } catch {
        $status = $null; $bodyText = ''
        if ($_.Exception.PSObject.Properties['Response'] -and $_.Exception.Response) { $status = [int]$_.Exception.Response.StatusCode }
        if ($_.ErrorDetails) { $bodyText = $_.ErrorDetails.Message }
        $err = [System.Exception]::new("Device Portal POST $Path failed: HTTP $status $bodyText ($($_.Exception.Message))")
        $err.Data['Status'] = $status
        throw $err
    }
}

function Get-DpStatus($ErrorRecord) {
    # HTTP status carried by an Invoke-Dp failure (or $null).
    $e = if ($ErrorRecord.Exception) { $ErrorRecord.Exception } else { $ErrorRecord }
    if ($e.Data -and $e.Data.Contains('Status')) { return $e.Data['Status'] }
    return $null
}

function ConvertTo-VersionString($v) {
    # Device Portal reports Version as {Major,Minor,Build,Revision}.
    if ($v -is [string]) { return $v }
    return '{0}.{1}.{2}.{3}' -f $v.Major, $v.Minor, $v.Build, $v.Revision
}

function Get-InstalledPackage {
    # Looks up our package in Device Portal's package list. Returns an object with
    # Name/PackageFullName/PackageFamilyName/PackageRelativeId/VersionString, or $null.
    param([Parameter(Mandatory)] $Xbox)
    $r = Invoke-Dp $Xbox '/api/app/packagemanager/packages'
    # Device Portal's .Name is the DISPLAY name (e.g. "xbox-rl"); the manifest identity
    # (xbox-rl.host) is the PackageFullName prefix. Match on that, not the display name.
    $m = @($r.InstalledPackages | Where-Object { $_.PackageFullName -like "$($script:PackageName)_*" })
    if ($m.Count -eq 0) { return $null }
    $m | ForEach-Object { $_ | Add-Member -NotePropertyName VersionString -NotePropertyValue (ConvertTo-VersionString $_.Version) -Force -PassThru } |
        Sort-Object { [version]$_.VersionString } -Descending | Select-Object -First 1
}

function Get-RequiredPackage {
    param([Parameter(Mandatory)] $Xbox)
    $p = Get-InstalledPackage $Xbox
    if (-not $p) { throw "$script:PackageName is not installed on the console. Run: pwsh tools/deploy.ps1" }
    return $p
}

function Get-DpFileUrl([string]$FullName, [string]$RemoteDir, [string]$FileName, [string]$Endpoint = 'file', [string]$KnownFolder = 'LocalAppData') {
    # RemoteDir like  \LocalState  or  \LocalState\rootfs\game\Binaries
    # KnownFolder DevelopmentFiles: RemoteDir like \LooseApps\xbox-rl (no package, FullName unused)
    $pkg = if ($KnownFolder -eq 'LocalAppData') { "&packagefullname=$([uri]::EscapeDataString($FullName))" } else { '' }
    $u = "/api/filesystem/apps/${Endpoint}?knownfolderid=$KnownFolder$pkg&path=$([uri]::EscapeDataString($RemoteDir))"
    if ($FileName) { $u += "&filename=$([uri]::EscapeDataString($FileName))" }
    return $u
}

function Receive-DpFile {
    # Downloads <RemoteDir>\<FileName> from the app's local data. The file read is
    # authoritative; the sibling directory *listing* is known to report stale sizes.
    param($Xbox, [string]$FullName, [string]$RemoteDir, [string]$FileName, [string]$OutFile)
    New-Item -ItemType Directory -Force (Split-Path $OutFile -Parent) | Out-Null
    Invoke-Dp $Xbox (Get-DpFileUrl $FullName $RemoteDir $FileName) -OutFile $OutFile -TimeoutSec 600 | Out-Null
}

function Send-DpFile {
    # Uploads one file into <RemoteDir> (which must exist; see New-DpDirectory).
    # Streams from disk (game files are up to ~1 GB) with a multipart body shaped
    # like Invoke-DpMultipart's: the real console's filesystem endpoint returns 500
    # for PowerShell's -Form body (filename*= parameter / quoted boundary).
    param($Xbox, [string]$FullName, [string]$RemoteDir, [string]$LocalFile, [string]$KnownFolder = 'LocalAppData')
    Initialize-DpSession $Xbox
    $path = Get-DpFileUrl $FullName $RemoteDir '' -KnownFolder $KnownFolder
    $name = [IO.Path]::GetFileName($LocalFile)
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.CookieContainer = $script:DpSession.Cookies
    $handler.ServerCertificateCustomValidationCallback = [System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
    $client = [System.Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromHours(6)
    $pw = $Xbox.Cred.GetNetworkCredential().Password
    $client.DefaultRequestHeaders.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new(
        'Basic', [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("$($Xbox.User):$pw")))
    $csrf = Get-DpCsrf; if ($csrf) { $client.DefaultRequestHeaders.Add('X-CSRF-Token', $csrf) }
    $fs = [IO.File]::OpenRead($LocalFile)
    try {
        $boundary = "----xrl$([guid]::NewGuid().ToString('N'))"
        $mp = [System.Net.Http.MultipartFormDataContent]::new($boundary)
        # .NET quotes the boundary parameter; send it bare like our working uploader.
        ($mp.Headers.ContentType.Parameters | Where-Object Name -eq 'boundary').Value = $boundary
        $part = [System.Net.Http.StreamContent]::new($fs, 1MB)
        $part.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream')
        $cd = [System.Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data')
        $cd.Name = "`"$name`""
        $cd.FileName = "`"$name`""          # plain filename only, no filename*=
        $part.Headers.ContentDisposition = $cd
        $mp.Add($part)
        $resp = $client.PostAsync("$($Xbox.Base)$path", $mp).GetAwaiter().GetResult()
        if (-not $resp.IsSuccessStatusCode) {
            $body = $resp.Content.ReadAsStringAsync().GetAwaiter().GetResult()
            $err = [System.Exception]::new("Device Portal POST $path failed: HTTP $([int]$resp.StatusCode) $body")
            $err.Data['Status'] = [int]$resp.StatusCode
            throw $err
        }
    } finally {
        $fs.Dispose(); $client.Dispose()
    }
}

function New-DpDirectory {
    # Creates \LocalState\a\b\c one level at a time (the upload endpoint does not
    # create missing directories). Existing levels are fine.
    param($Xbox, [string]$FullName, [string]$RemoteDir, [string]$KnownFolder = 'LocalAppData')
    $parts = @($RemoteDir.Trim('\').Split('\') | Where-Object { $_ })
    if ($parts.Count -lt 2) { return }          # \LocalState itself always exists
    $parent = '\' + $parts[0]
    for ($i = 1; $i -lt $parts.Count; $i++) {
        $u = Get-DpFileUrl $FullName $parent '' 'folder' $KnownFolder
        $u += "&newfoldername=$([uri]::EscapeDataString($parts[$i]))"
        try { Invoke-Dp $Xbox $u -Method POST | Out-Null }
        catch {
            # Already-exists is reported as an error by some builds; only fail if the
            # folder still is not there.
            $list = Invoke-Dp $Xbox (Get-DpFileUrl $FullName $parent '' 'files' $KnownFolder)
            if (-not ($list.Items | Where-Object { $_.Name -eq $parts[$i] -and $_.Type -eq 16 })) { throw }
        }
        $parent += '\' + $parts[$i]
    }
}

function Find-Msbuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found - is Visual Studio 2022 installed?' }
    $p = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $p) { throw 'MSBuild.exe not found via vswhere' }
    return $p
}

function Read-BuildInfo {
    $f = Join-Path $script:RepoRoot 'host\build\package\latest.json'
    if (-not (Test-Path $f)) { throw "No build recorded ($f). Run: pwsh tools/build.ps1" }
    return Get-Content $f -Raw | ConvertFrom-Json
}
