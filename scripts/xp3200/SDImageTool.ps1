# XP3200 SD card image tool (Windows GUI wrapper around WSL mtools).
#
# Usage (Windows PowerShell):
#   powershell -ExecutionPolicy Bypass -File SDImageTool.ps1
#
# Requires: WSL with dosfstools (mkfs.vfat) and mtools (mcopy/mdel/mdeltree).

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

function ConvertTo-WslPath([string]$path) {
    $p = $path -replace '\\', '/'
    if ($p -match '^([A-Za-z]):/(.*)$') {
        return "/mnt/" + $matches[1].ToLower() + "/" + $matches[2]
    }
    return $p
}

function Quote-Bash([string]$s) {
    return "'" + ($s -replace "'", "'\''") + "'"
}

$script:ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:WslTools = (ConvertTo-WslPath $script:ScriptDir) + "/sd_image.sh"
$script:Sources = New-Object System.Collections.ArrayList
$script:ImgEntries = New-Object System.Collections.ArrayList

function Run-Wsl([string]$cmd) {
    $tmp = [System.IO.Path]::GetTempFileName()
    try {
        $argLine = 'bash -lc "' + $cmd + '"'
        $p = Start-Process -FilePath "wsl.exe" -ArgumentList $argLine `
             -RedirectStandardOutput $tmp -RedirectStandardError ($tmp + ".err") `
             -NoNewWindow -PassThru
        while (-not $p.HasExited) {
            [System.Windows.Forms.Application]::DoEvents()
            Start-Sleep -Milliseconds 100
        }
        $text = ""
        if (Test-Path $tmp) { $text += Get-Content $tmp -Raw }
        if (Test-Path ($tmp + ".err")) { $text += Get-Content ($tmp + ".err") -Raw }
        return $text
    } finally {
        Remove-Item $tmp, ($tmp + ".err") -ErrorAction SilentlyContinue
    }
}

$form = New-Object System.Windows.Forms.Form
$form.Text = "XP3200 SD 卡镜像工具"
$form.Size = New-Object System.Drawing.Size(780, 700)
$form.MinimumSize = New-Object System.Drawing.Size(720, 600)
$form.StartPosition = "CenterScreen"
$form.Font = New-Object System.Drawing.Font("Microsoft YaHei UI", 9)
$form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::None

$root = New-Object System.Windows.Forms.TableLayoutPanel
$root.Dock = "Fill"
$root.ColumnCount = 1
$root.RowCount = 6
$root.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle("Percent", 100))) | Out-Null
$root.RowStyles.Add((New-Object System.Windows.Forms.RowStyle("Absolute", 196))) | Out-Null
$root.RowStyles.Add((New-Object System.Windows.Forms.RowStyle("Absolute", 42))) | Out-Null
$root.RowStyles.Add((New-Object System.Windows.Forms.RowStyle("Percent", 100))) | Out-Null
$root.RowStyles.Add((New-Object System.Windows.Forms.RowStyle("Absolute", 42))) | Out-Null
$root.RowStyles.Add((New-Object System.Windows.Forms.RowStyle("Absolute", 46))) | Out-Null
$root.RowStyles.Add((New-Object System.Windows.Forms.RowStyle("Absolute", 118))) | Out-Null
$form.Controls.Add($root)

# ---------------- top: mode + path + size/label ----------------
$top = New-Object System.Windows.Forms.TableLayoutPanel
$top.Dock = "Fill"
$top.ColumnCount = 3
$top.RowCount = 4
$top.Padding = New-Object System.Windows.Forms.Padding(10, 6, 10, 0)
$top.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle("Absolute", 90)))  | Out-Null
$top.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle("Percent", 100)))  | Out-Null
$top.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle("Absolute", 110))) | Out-Null
$top.RowStyles.Add((New-Object System.Windows.Forms.RowStyle("Absolute", 34))) | Out-Null
$top.RowStyles.Add((New-Object System.Windows.Forms.RowStyle("Absolute", 38))) | Out-Null
$top.RowStyles.Add((New-Object System.Windows.Forms.RowStyle("Absolute", 38))) | Out-Null
$top.RowStyles.Add((New-Object System.Windows.Forms.RowStyle("Absolute", 38))) | Out-Null

$modePanel = New-Object System.Windows.Forms.FlowLayoutPanel
$modePanel.Dock = "Fill"
$modePanel.FlowDirection = "LeftToRight"

$radioNew = New-Object System.Windows.Forms.RadioButton
$radioNew.Text = "新建镜像"
$radioNew.AutoSize = $true
$radioNew.Checked = $true
$radioNew.Margin = New-Object System.Windows.Forms.Padding(0, 6, 20, 0)

$radioEdit = New-Object System.Windows.Forms.RadioButton
$radioEdit.Text = "编辑已有镜像"
$radioEdit.AutoSize = $true
$radioEdit.Margin = New-Object System.Windows.Forms.Padding(0, 6, 0, 0)

$modePanel.Controls.Add($radioNew)
$modePanel.Controls.Add($radioEdit)

$lblPath = New-Object System.Windows.Forms.Label
$lblPath.Text = "镜像路径："
$lblPath.TextAlign = "MiddleLeft"
$lblPath.Dock = "Fill"

$txtPath = New-Object System.Windows.Forms.TextBox
$txtPath.Dock = "Fill"
$txtPath.Margin = New-Object System.Windows.Forms.Padding(0, 7, 6, 7)
$txtPath.Text = (Join-Path ([Environment]::GetFolderPath("MyDocuments")) "sdcard_128M.img")

$btnPath = New-Object System.Windows.Forms.Button
$btnPath.Text = "浏览..."
$btnPath.Dock = "Fill"
$btnPath.Margin = New-Object System.Windows.Forms.Padding(0, 5, 0, 5)

$lblSize = New-Object System.Windows.Forms.Label
$lblSize.Text = "容量："
$lblSize.TextAlign = "MiddleLeft"
$lblSize.Dock = "Fill"

$cmbSize = New-Object System.Windows.Forms.ComboBox
$cmbSize.DropDownStyle = "DropDownList"
$cmbSize.Dock = "Fill"
$cmbSize.Margin = New-Object System.Windows.Forms.Padding(0, 7, 6, 7)
[void]$cmbSize.Items.AddRange(@("64 MB", "128 MB", "256 MB", "512 MB"))
$cmbSize.SelectedIndex = 1

$lblLabel = New-Object System.Windows.Forms.Label
$lblLabel.Text = "卷标："
$lblLabel.TextAlign = "MiddleLeft"
$lblLabel.Dock = "Fill"

$txtLabel = New-Object System.Windows.Forms.TextBox
$txtLabel.Dock = "Fill"
$txtLabel.Margin = New-Object System.Windows.Forms.Padding(0, 7, 6, 7)
$txtLabel.Text = "SD CARD"

$top.Controls.Add($modePanel, 0, 0)
$top.SetColumnSpan($modePanel, 3)
$top.Controls.Add($lblPath, 0, 1)
$top.Controls.Add($txtPath, 1, 1)
$top.Controls.Add($btnPath, 2, 1)
$top.Controls.Add($lblSize, 0, 2)
$top.Controls.Add($cmbSize, 1, 2)
$top.Controls.Add($lblLabel, 0, 3)
$top.Controls.Add($txtLabel, 1, 3)
$root.Controls.Add($top, 0, 0)

# ---------------- source buttons ----------------
$srcBar = New-Object System.Windows.Forms.Panel
$srcBar.Dock = "Fill"

$btnAddFiles = New-Object System.Windows.Forms.Button
$btnAddFiles.Text = "添加文件..."
$btnAddFiles.Location = New-Object System.Drawing.Point(12, 6)
$btnAddFiles.Size = New-Object System.Drawing.Size(110, 28)

$btnAddFolder = New-Object System.Windows.Forms.Button
$btnAddFolder.Text = "添加文件夹..."
$btnAddFolder.Location = New-Object System.Drawing.Point(130, 6)
$btnAddFolder.Size = New-Object System.Drawing.Size(120, 28)

$btnRemoveSrc = New-Object System.Windows.Forms.Button
$btnRemoveSrc.Text = "移除选中"
$btnRemoveSrc.Location = New-Object System.Drawing.Point(258, 6)
$btnRemoveSrc.Size = New-Object System.Drawing.Size(90, 28)

$btnClearSrc = New-Object System.Windows.Forms.Button
$btnClearSrc.Text = "清空列表"
$btnClearSrc.Location = New-Object System.Drawing.Point(356, 6)
$btnClearSrc.Size = New-Object System.Drawing.Size(90, 28)

$srcBar.Controls.Add($btnAddFiles)
$srcBar.Controls.Add($btnAddFolder)
$srcBar.Controls.Add($btnRemoveSrc)
$srcBar.Controls.Add($btnClearSrc)
$root.Controls.Add($srcBar, 0, 1)

# ---------------- two lists ----------------
$lists = New-Object System.Windows.Forms.TableLayoutPanel
$lists.Dock = "Fill"
$lists.ColumnCount = 2
$lists.RowCount = 1
$lists.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle("Percent", 50))) | Out-Null
$lists.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle("Percent", 50))) | Out-Null

$grpSrc = New-Object System.Windows.Forms.GroupBox
$grpSrc.Text = "待添加的文件 / 文件夹"
$grpSrc.Dock = "Fill"
$grpSrc.Margin = New-Object System.Windows.Forms.Padding(10, 2, 5, 2)
$lstSrc = New-Object System.Windows.Forms.ListBox
$lstSrc.Dock = "Fill"
$lstSrc.HorizontalScrollbar = $true
$lstSrc.Margin = New-Object System.Windows.Forms.Padding(6, 16, 6, 6)
$grpSrc.Controls.Add($lstSrc)
$lists.Controls.Add($grpSrc, 0, 0)

$grpImg = New-Object System.Windows.Forms.GroupBox
$grpImg.Text = "镜像内的文件"
$grpImg.Dock = "Fill"
$grpImg.Margin = New-Object System.Windows.Forms.Padding(5, 2, 10, 2)
$lstImg = New-Object System.Windows.Forms.ListBox
$lstImg.Dock = "Fill"
$lstImg.HorizontalScrollbar = $true
$lstImg.Margin = New-Object System.Windows.Forms.Padding(6, 16, 6, 6)
$grpImg.Controls.Add($lstImg)
$lists.Controls.Add($grpImg, 1, 0)
$root.Controls.Add($lists, 0, 2)

# ---------------- image buttons ----------------
$imgBar = New-Object System.Windows.Forms.Panel
$imgBar.Dock = "Fill"

$btnRefresh = New-Object System.Windows.Forms.Button
$btnRefresh.Text = "刷新镜像列表"
$btnRefresh.Location = New-Object System.Drawing.Point(12, 6)
$btnRefresh.Size = New-Object System.Drawing.Size(120, 28)

$btnDelete = New-Object System.Windows.Forms.Button
$btnDelete.Text = "删除选中"
$btnDelete.Location = New-Object System.Drawing.Point(140, 6)
$btnDelete.Size = New-Object System.Drawing.Size(100, 28)

$imgBar.Controls.Add($btnRefresh)
$imgBar.Controls.Add($btnDelete)
$root.Controls.Add($imgBar, 0, 3)

# ---------------- generate ----------------
$genBar = New-Object System.Windows.Forms.Panel
$genBar.Dock = "Fill"

$btnGen = New-Object System.Windows.Forms.Button
$btnGen.Text = "生成镜像"
$btnGen.Location = New-Object System.Drawing.Point(12, 6)
$btnGen.Size = New-Object System.Drawing.Size(130, 32)

$lblHint = New-Object System.Windows.Forms.Label
$lblHint.Text = "分区从 LBA 97 开始，FAT16；文件夹会作为子目录整体复制。"
$lblHint.AutoSize = $true
$lblHint.Location = New-Object System.Drawing.Point(155, 15)

$genBar.Controls.Add($btnGen)
$genBar.Controls.Add($lblHint)
$root.Controls.Add($genBar, 0, 4)

# ---------------- log ----------------
$log = New-Object System.Windows.Forms.TextBox
$log.Multiline = $true
$log.ScrollBars = "Vertical"
$log.ReadOnly = $true
$log.Dock = "Fill"
$log.Margin = New-Object System.Windows.Forms.Padding(10, 2, 10, 8)
$root.Controls.Add($log, 0, 5)

# ---------------- behavior ----------------
function UpdateSourceList {
    $lstSrc.Items.Clear()
    foreach ($s in $script:Sources) { [void]$lstSrc.Items.Add($s) }
}

function UpdateMode {
    $new = $radioNew.Checked
    $cmbSize.Enabled = $new
    $txtLabel.Enabled = $new
    $btnRefresh.Enabled = -not $new
    $btnDelete.Enabled = -not $new
    $btnGen.Text = if ($new) { "生成镜像" } else { "写入镜像" }
    $grpImg.Text = if ($new) { "镜像内的文件（编辑模式可查看）" } else { "镜像内的文件" }
}

function RefreshImage {
    if ($radioNew.Checked) { return }
    $img = ConvertTo-WslPath $txtPath.Text.Trim()
    $out = Run-Wsl ("bash " + (Quote-Bash $script:WslTools) + " list " + (Quote-Bash $img))
    $lstImg.Items.Clear()
    $script:ImgEntries.Clear()
    $line = ($out -split "`r?`n" | Where-Object { $_.TrimStart().StartsWith("[") } | Select-Object -First 1)
    if (-not $line) {
        $log.AppendText($out + "`r`n")
        return
    }
    try {
        $items = $line | ConvertFrom-Json
    } catch {
        $log.AppendText("解析列表失败：" + $line + "`r`n")
        return
    }
    foreach ($it in $items) {
        [void]$script:ImgEntries.Add($it)
        $suffix = if ($it.dir) { " [目录]" } else { "  (" + $it.size + " 字节)" }
        [void]$lstImg.Items.Add($it.path + $suffix)
    }
    $log.AppendText("镜像内共 " + $items.Count + " 项`r`n")
}

$radioNew.Add_CheckedChanged({
    if ($radioNew.Checked) { UpdateMode; $lstImg.Items.Clear(); $script:ImgEntries.Clear() }
})
$radioEdit.Add_CheckedChanged({
    if ($radioEdit.Checked) { UpdateMode; RefreshImage }
})

$btnPath.Add_Click({
    if ($radioEdit.Checked) {
        $dlg = New-Object System.Windows.Forms.OpenFileDialog
        $dlg.Filter = "IMG 镜像 (*.img)|*.img|所有文件 (*.*)|*.*"
        if ($dlg.ShowDialog() -eq "OK") {
            $txtPath.Text = $dlg.FileName
            RefreshImage
        }
    } else {
        $dlg = New-Object System.Windows.Forms.SaveFileDialog
        $dlg.Filter = "IMG 镜像 (*.img)|*.img|所有文件 (*.*)|*.*"
        $dlg.FileName = "sdcard_128M.img"
        if ($dlg.ShowDialog() -eq "OK") { $txtPath.Text = $dlg.FileName }
    }
})

$btnAddFiles.Add_Click({
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Multiselect = $true
    if ($dlg.ShowDialog() -eq "OK") {
        foreach ($f in $dlg.FileNames) { [void]$script:Sources.Add($f) }
        UpdateSourceList
    }
})

$btnAddFolder.Add_Click({
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    if ($dlg.ShowDialog() -eq "OK") {
        [void]$script:Sources.Add($dlg.SelectedPath)
        UpdateSourceList
    }
})

$btnRemoveSrc.Add_Click({
    for ($i = $lstSrc.SelectedIndices.Count - 1; $i -ge 0; $i--) {
        $idx = $lstSrc.SelectedIndices[$i]
        $script:Sources.RemoveAt($idx)
    }
    UpdateSourceList
})

$btnClearSrc.Add_Click({
    $script:Sources.Clear()
    UpdateSourceList
})

$btnRefresh.Add_Click({ RefreshImage })

$btnDelete.Add_Click({
    if ($radioNew.Checked) { return }
    if ($lstImg.SelectedIndex -lt 0) { return }
    $entry = $script:ImgEntries[$lstImg.SelectedIndex]
    $what = if ($entry.dir) { "目录（含全部内容）" } else { "文件" }
    $answer = [System.Windows.Forms.MessageBox]::Show(
        "确定删除" + $what + "`n" + $entry.path + " ?", "删除",
        [System.Windows.Forms.MessageBoxButtons]::YesNo)
    if ($answer -ne "Yes") { return }
    $img = ConvertTo-WslPath $txtPath.Text.Trim()
    $target = $entry.path + $(if ($entry.dir) { "/" } else { "" })
    $out = Run-Wsl ("bash " + (Quote-Bash $script:WslTools) + " delete " +
                    (Quote-Bash $img) + " " + (Quote-Bash $target))
    $log.AppendText($out)
    RefreshImage
})

$btnGen.Add_Click({
    $path = $txtPath.Text.Trim()
    if ($path -eq "") {
        [System.Windows.Forms.MessageBox]::Show("请填写镜像路径。")
        return
    }
    $wslPath = ConvertTo-WslPath $path
    $btnGen.Enabled = $false
    try {
        if ($radioNew.Checked) {
            $sizeText = $cmbSize.SelectedItem.ToString()
            $sizeMB = [int]($sizeText -replace ' .*$', '')
            $label = $txtLabel.Text.Trim()
            if ($label -eq "") { $label = "SD CARD" }
            $args = @("create", $wslPath, [string]$sizeMB, $label) +
                    ($script:Sources | ForEach-Object { ConvertTo-WslPath $_ })
            $quoted = $args | ForEach-Object { Quote-Bash $_ }
            $cmd = "bash " + (Quote-Bash $script:WslTools) + " " + ($quoted -join ' ')
            $log.Text = "执行：" + $cmd + "`r`n"
            $out = Run-Wsl $cmd
            $log.AppendText($out + "`r`n完成。")
            $radioEdit.Checked = $true
            RefreshImage
        } else {
            if ($script:Sources.Count -eq 0) {
                [System.Windows.Forms.MessageBox]::Show("请先添加要写入的文件或文件夹。")
                return
            }
            $args = @("add", $wslPath) + ($script:Sources | ForEach-Object { ConvertTo-WslPath $_ })
            $quoted = $args | ForEach-Object { Quote-Bash $_ }
            $cmd = "bash " + (Quote-Bash $script:WslTools) + " " + ($quoted -join ' ')
            $log.Text = "执行：" + $cmd + "`r`n"
            $out = Run-Wsl $cmd
            $log.AppendText($out + "`r`n完成。")
            $script:Sources.Clear()
            UpdateSourceList
            RefreshImage
        }
    } finally {
        $btnGen.Enabled = $true
    }
})

UpdateMode
[void]$form.ShowDialog()
