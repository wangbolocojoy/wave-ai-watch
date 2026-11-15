// WAVE 媒体压缩 · 把手表看的照片和视频批量压成可流畅播放的格式
//
// 原生 macOS 应用（AppKit），无第三方依赖，只调用系统里的 ffmpeg。
// 照片 → baseline JPEG（≤512 盒 / ≤240,000 像素）
// 视频 → MJPEG AVI（320×240 / 15 fps / 16 kHz 单声道 PCM 音轨）
//
// 构建：见同目录 build-app.sh
// 命令行：WaveMediaConverter --cli --input <路径> --output <路径> --preset standard

import AppKit

func echo(_ message: String) {
    FileHandle.standardOutput.write((message + "\n").data(using: .utf8)!)
}

// MARK: - 预设

struct Preset {
    let key: String
    let title: String
    let detail: String
    let photoBox: Int
    let photoQuality: Int
    let videoWidth: Int
    let videoHeight: Int
    let videoFps: Int
    let videoQuality: Int

    static let all: [Preset] = [
        Preset(key: "compact", title: "紧凑（省空间）", detail: "照片 384px · 视频 288×216@12fps",
               photoBox: 384, photoQuality: 6, videoWidth: 288, videoHeight: 216,
               videoFps: 12, videoQuality: 8),
        Preset(key: "standard", title: "标准（推荐）", detail: "照片 480px · 视频 320×240@15fps",
               photoBox: 480, photoQuality: 4, videoWidth: 320, videoHeight: 240,
               videoFps: 15, videoQuality: 7),
        Preset(key: "sharp", title: "清晰（占空间）", detail: "照片 512px · 视频 360×288@15fps",
               photoBox: 512, photoQuality: 3, videoWidth: 360, videoHeight: 288,
               videoFps: 15, videoQuality: 5),
    ]

    static func named(_ key: String) -> Preset {
        all.first { $0.key == key } ?? all[1]
    }
}

let photoExtensions: Set<String> = ["jpg", "jpeg", "png", "heic", "heif", "webp", "bmp", "tif", "tiff", "gif"]
let videoExtensions: Set<String> = ["mp4", "mov", "m4v", "mkv", "avi", "webm", "wmv", "flv", "3gp", "mts"]

// MARK: - 转换引擎

final class MediaConverter {
    private let fileManager = FileManager.default
    private(set) var ffmpegPath: String?

    init() {
        ffmpegPath = Self.findExecutable("ffmpeg")
    }

    static func findExecutable(_ name: String) -> String? {
        let candidates = [
            "/opt/homebrew/bin/\(name)",
            "/usr/local/bin/\(name)",
            "/usr/bin/\(name)",
        ]
        for path in candidates where FileManager.default.isExecutableFile(atPath: path) {
            return path
        }
        // App 从 Finder 启动时 PATH 很短，问一次登录 shell
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/sh")
        process.arguments = ["-lc", "command -v \(name)"]
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = FileHandle.nullDevice
        try? process.run()
        process.waitUntilExit()
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        if let output = String(data: data, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines),
           !output.isEmpty, FileManager.default.isExecutableFile(atPath: output) {
            return output
        }
        return nil
    }

    struct Summary {
        var photos = 0
        var videos = 0
        var failed = 0
        var bytesIn: Int64 = 0
        var bytesOut: Int64 = 0
    }

    func collectSources(_ inputs: [URL]) -> [URL] {
        var files: [URL] = []
        for item in inputs {
            var isDirectory: ObjCBool = false
            guard fileManager.fileExists(atPath: item.path, isDirectory: &isDirectory) else { continue }
            if isDirectory.boolValue {
                let enumerator = fileManager.enumerator(at: item, includingPropertiesForKeys: [.isRegularFileKey])
                while let url = enumerator?.nextObject() as? URL {
                    let ext = url.pathExtension.lowercased()
                    if photoExtensions.contains(ext) || videoExtensions.contains(ext) { files.append(url) }
                }
            } else {
                let ext = item.pathExtension.lowercased()
                if photoExtensions.contains(ext) || videoExtensions.contains(ext) { files.append(item) }
            }
        }
        return files.sorted { $0.path < $1.path }
    }

    private func run(_ tool: String, _ arguments: [String]) -> (Int32, String) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: tool)
        process.arguments = arguments
        let stderr = Pipe()
        process.standardError = stderr
        process.standardOutput = Pipe()
        do {
            try process.run()
        } catch {
            return (-1, error.localizedDescription)
        }
        let data = stderr.fileHandleForReading.readDataToEndOfFile()
        process.waitWhileExitSafe()
        return (process.terminationStatus, String(data: data, encoding: .utf8) ?? "")
    }

    private func uniqueURL(in directory: URL, stem: String, suffix: String) -> URL {
        var candidate = directory.appendingPathComponent(stem + suffix)
        var index = 2
        while fileManager.fileExists(atPath: candidate.path) {
            candidate = directory.appendingPathComponent("\(stem)-\(index)\(suffix)")
            index += 1
        }
        return candidate
    }

    private func fileSize(_ url: URL) -> Int64 {
        (try? fileManager.attributesOfItem(atPath: url.path)[.size] as? Int64) ?? 0
    }

    func convert(inputs: [URL], output: URL, preset: Preset,
                 onProgress: @escaping (Int, Int) -> Void,
                 onLog: @escaping (String) -> Void) -> Summary {
        var summary = Summary()
        let photosDir = output.appendingPathComponent("Photos")
        let videosDir = output.appendingPathComponent("Video")
        try? fileManager.createDirectory(at: photosDir, withIntermediateDirectories: true)
        try? fileManager.createDirectory(at: videosDir, withIntermediateDirectories: true)

        let sources = collectSources(inputs)
        guard !sources.isEmpty else {
            onLog("没有找到可转换的照片或视频。")
            return summary
        }
        onLog("共找到 \(sources.count) 个文件，预设：\(preset.title)")

        for (index, source) in sources.enumerated() {
            onProgress(index, sources.count)
            let ext = source.pathExtension.lowercased()
            let stem = source.deletingPathExtension().lastPathComponent
            let before = fileSize(source)

            if photoExtensions.contains(ext) {
                let target = uniqueURL(in: photosDir, stem: stem, suffix: ".jpg")
                if let message = convertPhoto(source, target: target, preset: preset) {
                    summary.failed += 1
                    onLog("[\(index + 1)/\(sources.count)] ✗ \(source.lastPathComponent)：\(message)")
                } else {
                    summary.photos += 1
                    summary.bytesIn += before
                    summary.bytesOut += fileSize(target)
                    onLog("[\(index + 1)/\(sources.count)] ✓ \(source.lastPathComponent) → \(target.lastPathComponent)")
                }
            } else if videoExtensions.contains(ext) {
                let target = uniqueURL(in: videosDir, stem: stem, suffix: ".avi")
                if let message = convertVideo(source, target: target, preset: preset) {
                    summary.failed += 1
                    onLog("[\(index + 1)/\(sources.count)] ✗ \(source.lastPathComponent)：\(message)")
                } else {
                    summary.videos += 1
                    summary.bytesIn += before
                    summary.bytesOut += fileSize(target)
                    onLog(String(format: "[%d/%d] ✓ %@ → %@（%.1f MB）",
                                 index + 1, sources.count, source.lastPathComponent,
                                 target.lastPathComponent, Double(fileSize(target)) / 1024 / 1024))
                }
            }
        }
        onProgress(sources.count, sources.count)
        return summary
    }

    private func convertPhoto(_ source: URL, target: URL, preset: Preset) -> String? {
        guard let ffmpeg = ffmpegPath else { return "找不到 ffmpeg" }
        var input = source
        var temporary: URL?
        if ["heic", "heif"].contains(source.pathExtension.lowercased()) {
            let tmp = fileManager.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".jpg")
            let (status, _) = run("/usr/bin/sips", ["-s", "format", "jpeg", "-s", "formatOptions", "best",
                                                    source.path, "--out", tmp.path])
            guard status == 0 else { return "HEIC 转换失败" }
            input = tmp
            temporary = tmp
        }
        defer { if let temporary { try? fileManager.removeItem(at: temporary) } }

        let filter = "scale=\(preset.photoBox):\(preset.photoBox):force_original_aspect_ratio=decrease:flags=lanczos,format=yuvj420p"
        let (status, message) = run(ffmpeg, [
            "-y", "-hide_banner", "-loglevel", "error",
            "-i", input.path,
            "-vf", filter,
            "-frames:v", "1",
            "-q:v", String(preset.photoQuality),
            "-map_metadata", "-1",
            target.path,
        ])
        if status != 0 { return message.isEmpty ? "ffmpeg 转换失败" : message }
        return nil
    }

    private func convertVideo(_ source: URL, target: URL, preset: Preset) -> String? {
        guard let ffmpeg = ffmpegPath else { return "找不到 ffmpeg" }
        let filter = "scale=\(preset.videoWidth):\(preset.videoHeight):force_original_aspect_ratio=decrease:flags=lanczos,"
            + "scale=trunc(iw/2)*2:trunc(ih/2)*2,fps=\(preset.videoFps)"
        let (status, message) = run(ffmpeg, [
            "-y", "-hide_banner", "-loglevel", "error",
            "-i", source.path,
            "-vf", filter,
            "-c:v", "mjpeg",
            "-q:v", String(preset.videoQuality),
            "-pix_fmt", "yuvj420p",
            // 音频保留为 16 kHz 单声道 PCM，手表会和画面一起播放
            "-c:a", "pcm_s16le", "-ar", "16000", "-ac", "1",
            "-map_metadata", "-1",
            "-f", "avi",
            target.path,
        ])
        if status != 0 {
            let trimmed = message.isEmpty ? "ffmpeg 转换失败" : message
            return String(trimmed.prefix(160))
        }
        return nil
    }
}

extension Process {
    func waitWhileExitSafe() {
        waitUntilExit()
    }
}

// MARK: - 拖放区域

final class DropView: NSView {
    var onDrop: (([URL]) -> Void)?
    private var highlighted = false

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        registerForDraggedTypes([.fileURL])
        wantsLayer = true
        layer?.cornerRadius = 12
        layer?.borderWidth = 1.5
        layer?.borderColor = NSColor.separatorColor.cgColor
    }

    required init?(coder: NSCoder) { fatalError() }

    private func updateAppearance() {
        layer?.borderColor = (highlighted ? NSColor.controlAccentColor : NSColor.separatorColor).cgColor
        layer?.backgroundColor = (highlighted ? NSColor.controlAccentColor.withAlphaComponent(0.08)
                                              : NSColor.clear).cgColor
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        highlighted = true
        updateAppearance()
        return .copy
    }

    override func draggingExited(_ sender: NSDraggingInfo?) {
        highlighted = false
        updateAppearance()
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        highlighted = false
        updateAppearance()
        guard let urls = sender.draggingPasteboard.readObjects(
            forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL],
              !urls.isEmpty else { return false }
        onDrop?(urls)
        return true
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        let text = "把照片 / 视频文件夹拖到这里" as NSString
        let attributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 15, weight: .medium),
            .foregroundColor: NSColor.secondaryLabelColor,
        ]
        let size = text.size(withAttributes: attributes)
        text.draw(at: NSPoint(x: (bounds.width - size.width) / 2,
                              y: (bounds.height - size.height) / 2), withAttributes: attributes)
    }
}

// MARK: - 主界面

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow!
    private var inputPathLabel: NSTextField!
    private var outputPathLabel: NSTextField!
    private var presetPopup: NSPopUpButton!
    private var startButton: NSButton!
    private var revealButton: NSButton!
    private var progressBar: NSProgressIndicator!
    private var statusLabel: NSTextField!
    private var logView: NSTextView!
    private var dropView: DropView!

    private let converter = MediaConverter()
    private var inputs: [URL] = []
    private var outputDirectory = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Desktop/watch-media")
    private var busy = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildWindow()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        if converter.ffmpegPath == nil {
            appendLog("⚠️ 没有找到 ffmpeg。请先在终端执行：brew install ffmpeg，然后重新打开本应用。")
        } else {
            appendLog("ffmpeg: \(converter.ffmpegPath ?? "")")
            appendLog("选择或拖入照片/视频文件夹，然后点「开始转换」。")
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    private func buildWindow() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 780, height: 660),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        window.title = "WAVE 媒体压缩 · 手表照片/视频"
        window.minSize = NSSize(width: 660, height: 600)
        window.center()

        let content = NSView()
        window.contentView = content

        let title = NSTextField(labelWithString: "把照片和视频压成手表能流畅查看的格式")
        title.font = NSFont.systemFont(ofSize: 20, weight: .bold)
        title.translatesAutoresizingMaskIntoConstraints = false

        let subtitle = NSTextField(
            labelWithString: "照片 → baseline JPEG（≤480px）   ·   视频 → MJPEG AVI（320×240 / 15fps / 无音轨）")
        subtitle.font = NSFont.systemFont(ofSize: 12)
        subtitle.textColor = .secondaryLabelColor
        subtitle.translatesAutoresizingMaskIntoConstraints = false

        dropView = DropView(frame: .zero)
        dropView.translatesAutoresizingMaskIntoConstraints = false
        dropView.onDrop = { [weak self] urls in
            guard let self else { return }
            self.inputs = urls
            self.inputPathLabel.stringValue = urls.count == 1 ? urls[0].path : "\(urls.count) 个项目"
            self.appendLog("已选择输入：\(self.inputPathLabel.stringValue)")
        }

        let (inputRow, inputField) = pathRow(label: "输入", action: #selector(chooseInput))
        inputPathLabel = inputField
        inputRow.translatesAutoresizingMaskIntoConstraints = false
        let (outputRow, outputField) = pathRow(label: "输出", action: #selector(chooseOutput))
        outputPathLabel = outputField
        outputPathLabel.stringValue = outputDirectory.path
        outputRow.translatesAutoresizingMaskIntoConstraints = false

        presetPopup = NSPopUpButton()
        for preset in Preset.all {
            presetPopup.addItem(withTitle: "\(preset.title)  ·  \(preset.detail)")
        }
        presetPopup.selectItem(at: 1)
        let presetCaption = NSTextField(labelWithString: "预设")
        presetCaption.font = NSFont.systemFont(ofSize: 12, weight: .semibold)
        let presetRow = NSStackView(views: [presetCaption, presetPopup])
        presetRow.orientation = .horizontal
        presetRow.spacing = 8
        presetRow.translatesAutoresizingMaskIntoConstraints = false

        startButton = NSButton(title: "开始转换", target: self, action: #selector(startConversion))
        startButton.bezelStyle = .rounded
        startButton.controlSize = .large
        startButton.keyEquivalent = "\r"
        startButton.widthAnchor.constraint(greaterThanOrEqualToConstant: 120).isActive = true

        revealButton = NSButton(title: "在 Finder 中显示", target: self, action: #selector(revealOutput))
        revealButton.bezelStyle = .rounded
        revealButton.isEnabled = false

        let buttonRow = NSStackView(views: [startButton, revealButton])
        buttonRow.orientation = .horizontal
        buttonRow.spacing = 12
        buttonRow.translatesAutoresizingMaskIntoConstraints = false

        progressBar = NSProgressIndicator()
        progressBar.isIndeterminate = false
        progressBar.minValue = 0
        progressBar.maxValue = 1
        progressBar.translatesAutoresizingMaskIntoConstraints = false

        statusLabel = NSTextField(labelWithString: "就绪")
        statusLabel.font = NSFont.systemFont(ofSize: 12)
        statusLabel.textColor = .secondaryLabelColor
        statusLabel.translatesAutoresizingMaskIntoConstraints = false

        let logScroll = NSScrollView()
        logScroll.hasVerticalScroller = true
        logScroll.borderType = .bezelBorder
        logScroll.translatesAutoresizingMaskIntoConstraints = false
        logView = NSTextView(frame: NSRect(x: 0, y: 0, width: 720, height: 240))
        logView.isEditable = false
        logView.isVerticallyResizable = true
        logView.isHorizontallyResizable = false
        logView.autoresizingMask = [.width]
        logView.textContainer?.widthTracksTextView = true
        logView.textContainerInset = NSSize(width: 8, height: 8)
        logView.font = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
        logScroll.documentView = logView

        let stackedViews: [NSView] = [title, subtitle, dropView!, inputRow, outputRow, presetRow,
                                      progressBar, statusLabel, logScroll, buttonRow]
        for view in stackedViews {
            content.addSubview(view)
        }

        NSLayoutConstraint.activate([
            title.topAnchor.constraint(equalTo: content.topAnchor, constant: 20),
            title.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 22),
            title.trailingAnchor.constraint(lessThanOrEqualTo: content.trailingAnchor, constant: -22),

            subtitle.topAnchor.constraint(equalTo: title.bottomAnchor, constant: 6),
            subtitle.leadingAnchor.constraint(equalTo: title.leadingAnchor),
            subtitle.trailingAnchor.constraint(lessThanOrEqualTo: content.trailingAnchor, constant: -22),

            dropView.topAnchor.constraint(equalTo: subtitle.bottomAnchor, constant: 14),
            dropView.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 22),
            dropView.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -22),
            dropView.heightAnchor.constraint(equalToConstant: 92),

            inputRow.topAnchor.constraint(equalTo: dropView.bottomAnchor, constant: 14),
            inputRow.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 22),
            inputRow.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -22),

            outputRow.topAnchor.constraint(equalTo: inputRow.bottomAnchor, constant: 8),
            outputRow.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 22),
            outputRow.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -22),

            presetRow.topAnchor.constraint(equalTo: outputRow.bottomAnchor, constant: 10),
            presetRow.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 22),
            presetRow.trailingAnchor.constraint(lessThanOrEqualTo: content.trailingAnchor, constant: -22),

            progressBar.topAnchor.constraint(equalTo: presetRow.bottomAnchor, constant: 16),
            progressBar.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 22),
            progressBar.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -22),
            progressBar.heightAnchor.constraint(equalToConstant: 6),

            logScroll.topAnchor.constraint(equalTo: progressBar.bottomAnchor, constant: 12),
            logScroll.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 22),
            logScroll.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -22),
            logScroll.bottomAnchor.constraint(equalTo: statusLabel.topAnchor, constant: -10),

            statusLabel.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 22),
            statusLabel.bottomAnchor.constraint(equalTo: buttonRow.topAnchor, constant: -10),

            buttonRow.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 22),
            buttonRow.trailingAnchor.constraint(lessThanOrEqualTo: content.trailingAnchor, constant: -22),
            buttonRow.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -20),
        ])
    }

    private func pathRow(label: String, action: Selector) -> (NSStackView, NSTextField) {
        let caption = NSTextField(labelWithString: label)
        caption.font = NSFont.systemFont(ofSize: 12, weight: .semibold)
        caption.widthAnchor.constraint(equalToConstant: 28).isActive = true

        let field = NSTextField(labelWithString: "")
        field.lineBreakMode = .byTruncatingMiddle
        field.font = NSFont.systemFont(ofSize: 12)
        field.textColor = .labelColor
        field.isSelectable = true

        let choose = NSButton(title: "选择…", target: self, action: action)
        choose.bezelStyle = .rounded

        let row = NSStackView(views: [caption, field, choose])
        row.orientation = .horizontal
        row.spacing = 8
        field.setContentHuggingPriority(.defaultLow, for: .horizontal)
        return (row, field)
    }

    @objc private func chooseInput() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = true
        panel.message = "选择照片/视频所在文件夹（可多选）"
        guard panel.runModal() == .OK else { return }
        inputs = panel.urls
        inputPathLabel.stringValue = inputs.count == 1 ? inputs[0].path : "\(inputs.count) 个项目"
        appendLog("已选择输入：\(inputPathLabel.stringValue)")
    }

    @objc private func chooseOutput() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.message = "选择输出文件夹（会在这里生成 Photos/ 和 Video/）"
        panel.directoryURL = outputDirectory.deletingLastPathComponent()
        guard panel.runModal() == .OK, let url = panel.url else { return }
        outputDirectory = url
        outputPathLabel.stringValue = url.path
    }

    @objc private func startConversion() {
        guard !busy else { return }
        guard converter.ffmpegPath != nil else {
            alert("缺少 ffmpeg", "请先在终端执行：brew install ffmpeg")
            return
        }
        guard !inputs.isEmpty else {
            alert("还没有选择输入", "把照片/视频文件夹拖进窗口，或点「选择…」。")
            return
        }
        busy = true
        startButton.isEnabled = false
        revealButton.isEnabled = false
        progressBar.maxValue = 1
        progressBar.doubleValue = 0
        statusLabel.stringValue = "正在转换…"
        logView.string = ""

        let preset = Preset.all[max(0, min(presetPopup.indexOfSelectedItem, Preset.all.count - 1))]
        let output = outputDirectory
        let inputsCopy = inputs

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }
            let summary = self.converter.convert(
                inputs: inputsCopy, output: output, preset: preset,
                onProgress: { done, total in
                    DispatchQueue.main.async {
                        self.progressBar.maxValue = Double(max(total, 1))
                        self.progressBar.doubleValue = Double(done)
                        self.statusLabel.stringValue = "进度 \(done)/\(total)"
                    }
                },
                onLog: { message in
                    DispatchQueue.main.async { self.appendLog(message) }
                })

            DispatchQueue.main.async {
                self.busy = false
                self.startButton.isEnabled = true
                self.progressBar.doubleValue = self.progressBar.maxValue
                let outMB = Double(summary.bytesOut) / 1024 / 1024
                let inMB = Double(summary.bytesIn) / 1024 / 1024
                self.appendLog("")
                self.appendLog(String(format: "完成：照片 %d 张，视频 %d 个，失败 %d 个",
                                      summary.photos, summary.videos, summary.failed))
                self.appendLog(String(format: "输出 %.1f MB（原始 %.1f MB）→ %@",
                                      outMB, inMB, output.path))
                self.appendLog("把输出文件夹里的 Photos、Video 拷到 TF 卡根目录即可。")
                self.statusLabel.stringValue = summary.failed == 0 ? "完成" : "完成（有失败项）"
                self.revealButton.isEnabled = summary.photos + summary.videos > 0
            }
        }
    }

    @objc private func revealOutput() {
        NSWorkspace.shared.selectFile(nil, inFileViewerRootedAtPath: outputDirectory.path)
    }

    private func appendLog(_ message: String) {
        logView.string += (logView.string.isEmpty ? "" : "\n") + message
        logView.scrollToEndOfDocument(nil)
    }

    private func alert(_ title: String, _ message: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.alertStyle = .warning
        alert.runModal()
    }
}

// MARK: - 入口（GUI / CLI）

let rawArguments = CommandLine.arguments
if let cliIndex = rawArguments.firstIndex(of: "--cli") {
    var cliInputs: [URL] = []
    var cliOutput = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Desktop/watch-media")
    var cliPreset = Preset.all[1]
    var index = cliIndex + 1
    while index < rawArguments.count {
        switch rawArguments[index] {
        case "--input":
            index += 1
            while index < rawArguments.count, !rawArguments[index].hasPrefix("--") {
                let path = NSString(string: rawArguments[index]).expandingTildeInPath
                cliInputs.append(URL(fileURLWithPath: path))
                index += 1
            }
            continue
        case "--output":
            index += 1
            if index < rawArguments.count {
                cliOutput = URL(fileURLWithPath: NSString(string: rawArguments[index]).expandingTildeInPath)
            }
        case "--preset":
            index += 1
            if index < rawArguments.count { cliPreset = Preset.named(rawArguments[index]) }
        default:
            break
        }
        index += 1
    }
    let cliConverter = MediaConverter()
    let cliSummary = cliConverter.convert(
        inputs: cliInputs, output: cliOutput, preset: cliPreset,
        onProgress: { _, _ in },
        onLog: { echo($0) })
    echo("完成：照片 \(cliSummary.photos)，视频 \(cliSummary.videos)，失败 \(cliSummary.failed)")
    exit(cliSummary.failed == 0 ? 0 : 1)
}

let application = NSApplication.shared
application.setActivationPolicy(.regular)
let delegate = AppDelegate()
application.delegate = delegate
application.run()
