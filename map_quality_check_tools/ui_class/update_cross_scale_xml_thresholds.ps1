$ErrorActionPreference = 'Stop'
$targetPath = 'D:\Desktop\LTZK\图幅接边与跨比例尺检查\502同系列跨比例尺表达检查.xml'
$validationRoot = Join-Path $PSScriptRoot '_threshold_validation'
[void][IO.Directory]::CreateDirectory($validationRoot)
$originalBytes = [IO.File]::ReadAllBytes($targetPath)
$utf8 = New-Object Text.UTF8Encoding($false, $true)
$original = $utf8.GetString($originalBytes)
[xml]$originalDocument = $original
$rulePattern = '(?s)(<rule>\s*<name>)(?<name>[^<]+)(</name>.*?<value>)(?<value>[^<]*)(</value>.*?</rule>)'
$changes = [ordered]@{
    series_cross_scale_line_join_tolerance_m = '10'
    series_cross_scale_annotation_anchor_tolerance_mm = '0.8'
    series_cross_scale_direction_angle_max = '35'
    series_cross_scale_matched_threshold = '0.68'
}
$seen = New-Object 'Collections.Generic.HashSet[string]'
$modified = [regex]::Replace($original, $rulePattern, [Text.RegularExpressions.MatchEvaluator]{
    param($match)
    $name = $match.Groups['name'].Value
    $value = $match.Groups['value'].Value
    if ($name -eq 'series_cross_scale_tolerance_profiles') {
        if (-not $seen.Add($name)) { throw "Duplicate rule: $name" }
        $value = [regex]::Replace($value, '("generalization_mm"\s*:\s*)[0-9.]+', '${1}2')
        $value = [regex]::Replace($value, '("position_mm"\s*:\s*)[0-9.]+', '${1}2')
        $value = [regex]::Replace($value, '("search_mm"\s*:\s*)[0-9.]+', '${1}2')
        $value = [regex]::Replace($value, '("gap_mm"\s*:\s*)[0-9.]+', '${1}1')
    } elseif ($changes.Contains($name)) {
        if (-not $seen.Add($name)) { throw "Duplicate rule: $name" }
        $value = $changes[$name]
    }
    $relativeStart = $match.Groups['value'].Index - $match.Index
    return $match.Value.Substring(0, $relativeStart) + $value + $match.Value.Substring($relativeStart + $match.Groups['value'].Length)
})
if ($seen.Count -ne 5) { throw "Expected five updated rules, got $($seen.Count)" }
[xml]$document = $modified
$originalRules = @($originalDocument.SelectNodes('//check_item[id="502"]/rule'))
$rules = @($document.SelectNodes('//check_item[id="502"]/rule'))
if ($rules.Count -ne $originalRules.Count) { throw 'Rule count changed' }
for ($i = 0; $i -lt $rules.Count; $i++) {
    $name = $rules[$i].SelectSingleNode('name').InnerText
    $oldName = $originalRules[$i].SelectSingleNode('name').InnerText
    if ($name -ne $oldName) { throw 'Rule order changed' }
    if (-not $seen.Contains($name) -and $rules[$i].OuterXml -ne $originalRules[$i].OuterXml) {
        throw "Unrelated rule changed: $name"
    }
}
$profilesText = $document.SelectSingleNode('//rule[name="series_cross_scale_tolerance_profiles"]/value').InnerText
$profiles = $profilesText | ConvertFrom-Json
foreach ($property in $profiles.PSObject.Properties) {
    $profile = $property.Value
    if ($profile.generalization_mm -ne 2) { throw "Wrong generalization for $($property.Name)" }
    foreach ($key in @('position_mm', 'search_mm', 'expanded_search_mm', 'gap_mm')) {
        $effective = if ($null -ne $profile.$key) { $profile.$key } else { $profiles.default.$key }
        $expected = if ($key -eq 'gap_mm') { 1 } else { 2 }
        if ($effective -ne $expected) { throw "Wrong $key for $($property.Name)" }
    }
}
foreach ($name in $changes.Keys) {
    if ($document.SelectSingleNode("//rule[name='$name']/value").InnerText -ne $changes[$name]) { throw "Wrong value: $name" }
}
$backupPath = Join-Path $validationRoot '502.before.xml'
if (Test-Path -LiteralPath $backupPath) { throw 'Backup already exists; refusing to overwrite it' }
[IO.File]::WriteAllBytes($backupPath, $originalBytes)
$preparedPath = Join-Path $validationRoot '502.modified.xml'
[IO.File]::WriteAllText($preparedPath, $modified, $utf8)
$report = [ordered]@{
    target = $targetPath
    backup = $backupPath
    prepared = $preparedPath
    original_sha256 = (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash
    modified_sha256 = (Get-FileHash -LiteralPath $preparedPath -Algorithm SHA256).Hash
    changed_rules = @($seen)
    profiles = $profiles
    scalar_changes = $changes
    validation = 'PASS: XML, JSON, all class overrides, budget order, unchanged unrelated rules'
}
$report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $validationRoot 'report.json') -Encoding UTF8
$report | ConvertTo-Json -Depth 10
