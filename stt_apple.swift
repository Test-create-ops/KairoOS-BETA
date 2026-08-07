import Foundation
import AppKit
import Speech

// Uso: stt_apple <audiofile> [locale] [outfile]
// Trascrive un file audio con il riconoscimento vocale di macOS (dettatura).
// Scrive il testo su stdout (e su outfile se dato); errori con prefisso "ERR:" su stderr.

let args = CommandLine.arguments
guard args.count >= 2 else {
    FileHandle.standardError.write(Data("ERR: usage: stt_apple <audiofile> [locale] [outfile]\n".utf8))
    exit(2)
}
let path = args[1]
let locale = args.count >= 3 ? args[2] : "it-IT"
let outfile = args.count >= 4 ? args[3] : nil
let url = URL(fileURLWithPath: path)

func writeOutput(_ text: String) {
    let data = Data((text + "\n").utf8)
    FileHandle.standardOutput.write(data)
    if let outfile {
        try? data.write(to: URL(fileURLWithPath: outfile))
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
app.activate(ignoringOtherApps: true)

func finish(_ text: String?, _ err: String?) {
    if let err {
        FileHandle.standardError.write(Data("ERR: \(err)\n".utf8))
        writeOutput("ERR: \(err)")
    }
    if let text {
        writeOutput(text)
    }
    app.terminate(nil)
}

SFSpeechRecognizer.requestAuthorization { status in
    DispatchQueue.main.async {
        guard status == .authorized else {
            finish(nil, "speech recognition not authorized (status \(status.rawValue)); abilita l'accesso per 'OreoAI Speech' in Impostazioni > Privacy e Sicurezza > Riconoscimento vocale")
            return
        }
        guard let recognizer = SFSpeechRecognizer(locale: Locale(identifier: locale)) else {
            finish(nil, "no recognizer for locale \(locale)")
            return
        }
        let request = SFSpeechURLRecognitionRequest(url: url)
        if recognizer.supportsOnDeviceRecognition {
            request.requiresOnDeviceRecognition = true
        }
        request.shouldReportPartialResults = false
        request.taskHint = .dictation
        recognizer.recognitionTask(with: request) { result, error in
            if let result = result, result.isFinal {
                finish(result.bestTranscription.formattedString, nil)
            } else if let error {
                finish(nil, error.localizedDescription)
            }
        }
    }
}

DispatchQueue.main.asyncAfter(deadline: .now() + 90) {
    finish(nil, "timeout")
}

app.run()
