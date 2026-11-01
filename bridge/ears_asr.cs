// ears_asr -- Sesame bridge-side speech recognition helper (v1.22 ears).
//
// Uses Windows' built-in System.Speech recognizer (en-US desktop
// recognizer, fully offline) -- no cloud, no downloads. Two modes:
//
//   ears_asr spot <wav> <phrase>   wake-phrase spotting over one window:
//       prints "MATCH conf=<0..1> text=<...>" when the phrase grammar
//       wins with usable confidence, else "NONE best=<...> conf=<...>".
//   ears_asr hear <wav>            free dictation of one utterance:
//       prints "TEXT conf=<0..1> text=<...>", or "NONE ..." when the
//       recognizer heard nothing it would stand behind.
//
// Input WAV: PCM 16-bit mono (16 kHz is what the board streams).
// Exit code is 0 whenever recognition RAN (even on NONE); 2 on errors.
// The caller (bridge.py) treats stdout as the only result channel.
using System;
using System.Globalization;
using System.IO;
using System.Speech.Recognition;

class EarsAsr
{
    static SpeechRecognitionEngine MakeEngine()
    {
        RecognizerInfo chosen = null;
        foreach (var ri in SpeechRecognitionEngine.InstalledRecognizers())
        {
            if (ri.Culture != null && ri.Culture.Name == "en-US") { chosen = ri; break; }
        }
        var eng = chosen != null
            ? new SpeechRecognitionEngine(chosen)
            : new SpeechRecognitionEngine();
        eng.InitialSilenceTimeout = TimeSpan.FromSeconds(3);
        eng.BabbleTimeout = TimeSpan.FromSeconds(4);
        eng.EndSilenceTimeout = TimeSpan.FromMilliseconds(250);
        eng.EndSilenceTimeoutAmbiguous = TimeSpan.FromMilliseconds(250);
        return eng;
    }

    static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.WriteLine("ERR usage: ears_asr <spot|hear> <wav> [phrase]");
            return 2;
        }
        string mode = args[0];
        string wav = args[1];
        try
        {
            using (var eng = MakeEngine())
            {
                if (mode == "spot")
                {
                    if (args.Length < 3) { Console.WriteLine("ERR spot needs a phrase"); return 2; }
                    string phrase = args[2];
                    var gb = new GrammarBuilder();
                    gb.Culture = new CultureInfo("en-US");
                    gb.Append(new Choices(new SemanticResultValue(phrase, "WAKE")));
                    eng.LoadGrammar(new Grammar(gb) { Name = "wake" });
                }
                else if (mode == "hear")
                {
                    var dg = new DictationGrammar { Name = "dictation" };
                    eng.LoadGrammar(dg);
                }
                else
                {
                    Console.WriteLine("ERR unknown mode " + mode);
                    return 2;
                }
                using (var fs = new FileStream(wav, FileMode.Open, FileAccess.Read))
                {
                    eng.SetInputToWaveStream(fs);
                    RecognitionResult r = eng.Recognize();
                    if (r == null)
                    {
                        Console.WriteLine("NONE best= conf=0");
                        return 0;
                    }
                    string text = r.Text ?? "";
                    double conf = r.Confidence;
                    if (mode == "spot")
                    {
                        bool isWake = r.Grammar != null && r.Grammar.Name == "wake"
                                      && conf >= 0.45;
                        Console.WriteLine(isWake
                            ? "MATCH conf=" + conf.ToString("0.00", CultureInfo.InvariantCulture) + " text=" + text
                            : "NONE best=" + text + " conf=" + conf.ToString("0.00", CultureInfo.InvariantCulture));
                    }
                    else
                    {
                        if (text.Length == 0)
                            Console.WriteLine("NONE best= conf=" + conf.ToString("0.00", CultureInfo.InvariantCulture));
                        else
                            Console.WriteLine("TEXT conf=" + conf.ToString("0.00", CultureInfo.InvariantCulture) + " text=" + text);
                    }
                    return 0;
                }
            }
        }
        catch (Exception e)
        {
            Console.WriteLine("ERR " + e.GetType().Name + ": " + e.Message);
            return 2;
        }
    }
}
