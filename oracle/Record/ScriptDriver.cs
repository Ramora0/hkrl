using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using HKOracle.Env;
using Newtonsoft.Json.Linq;
using UnityEngine;

namespace HKOracle.Record
{
	// Drives TrainingEnv from a corpus file instead of the Python trainer
	// (docs/trace-format.md §Corpus file). Installed when HK_ORACLE_SCRIPT is set (Mode.cs).
	//
	// Socket.Sink diverts every outgoing reply to OnReply and makes
	// WebsocketEnv.Connect() a no-op (oracle/Env/WebsocketEnv.cs), so the run
	// needs no server. Requests are fed back through socket.UnreadMessages, which
	// is exactly where the websocket receive callback would have put them.
	public static class ScriptDriver
	{
		public static bool Active { get; private set; }
		public static string ScriptPath { get; private set; }
		public static string ScriptSha256 { get; private set; }
		public static string CorpusName { get; private set; }
		public static string Level { get; private set; }
		public static int FramesPerWait { get; private set; }

		// The trainer's time_scale, so the reset message matches what it sends.
		// TrainingEnv ignores the field (it pins Time.timeScale = 1 and uses
		// captureDeltaTime).
		private const int kTimeScale = 3;

		private static int[][] _steps = new int[0][];
		private static int _next;
		private static bool _finished;
		private static DriverBehaviour _mb;

		private static void Log(string m) => HKOracle.Instance.Log($"[Script] {m}");

		public static void Install()
		{
			string path = Mode.ScriptPath;
			string text;
			try { text = File.ReadAllText(path); }
			catch (Exception e)
			{
				Log($"FATAL cannot read corpus '{path}': {HKOracle.DescribeException(e)}");
				return;
			}

			try
			{
				var root = JObject.Parse(text);
				CorpusName = (string)root["name"] ?? Path.GetFileNameWithoutExtension(path);
				Level = (string)root["level"];
				FramesPerWait = root["frames_per_wait"] != null
					? (int)root["frames_per_wait"] : 5;
				var steps = (JArray)root["steps"];
				if (string.IsNullOrEmpty(Level) || steps == null || steps.Count == 0)
				{
					Log($"FATAL corpus '{path}' has no level or no steps");
					return;
				}
				_steps = new int[steps.Count][];
				for (int i = 0; i < steps.Count; i++)
				{
					var a = (JArray)steps[i];
					if (a == null || a.Count != 4)
					{
						Log($"FATAL corpus step {i} is not a 4-int array");
						return;
					}
					_steps[i] = new[] { (int)a[0], (int)a[1], (int)a[2], (int)a[3] };
				}
				if (root["seed"] != null && root["seed"].Type != JTokenType.Null)
					Log($"corpus seed={root["seed"]} (informational; HK_ORACLE_SEED is "
						+ "the authority — docs/trace-format.md §Oracle environment variables)");
			}
			catch (Exception e)
			{
				Log($"FATAL corpus parse failed: {HKOracle.DescribeException(e)}");
				return;
			}

			ScriptPath = path;
			ScriptSha256 = Sha256Hex(text);
			Active = true;

			var go = new GameObject("HKOracle.ScriptDriver");
			UnityEngine.Object.DontDestroyOnLoad(go);
			go.hideFlags = HideFlags.HideAndDontSave;
			_mb = go.AddComponent<DriverBehaviour>();

			Socket.Sink = OnReply;
			Log($"corpus={CorpusName} path={path} sha256={ScriptSha256} "
				+ $"level={Level} frames_per_wait={FramesPerWait} steps={_steps.Length}");

			Enqueue(new Message { type = "init", data = new MessageData() });
		}

		private static void Enqueue(Message m)
		{
			var env = Hooks.Env;
			if (env == null || env.socket == null)
			{
				Log("FATAL Hooks.Env not set — cannot enqueue");
				return;
			}
			env.socket.UnreadMessages.Enqueue(m);
		}

		// Every reply TrainingEnv sends lands here (instead of the websocket).
		private static void OnReply(Message m)
		{
			try
			{
				if (_finished || m == null) return;
				switch (m.type)
				{
					case "init":
						Log($"init reply — requesting reset level={Level} fpw={FramesPerWait}");
						// The trainer's reset: pack_reset field order, eval=False,
						// force_full=False.
						Enqueue(new Message
						{
							type = "reset",
							data = new MessageData
							{
								level = Level,
								frames_per_wait = FramesPerWait,
								time_scale = kTimeScale,
								eval = false,
								force_full = false,
							}
						});
						break;

					case "reset":
						_next = 0;
						Log($"reset reply — driving {_steps.Length} steps");
						SendNextAction();
						break;

					case "step":
						bool done = m.data != null && m.data.done == true;
						if (done)
						{
							Log($"done=true info={m.data.info ?? ""} at step {_next}/{_steps.Length}");
							Finish("episode done");
							break;
						}
						_next++;
						if (_next % 50 == 0) Log($"step {_next}/{_steps.Length}");
						if (_next >= _steps.Length) { Finish("steps exhausted"); break; }
						SendNextAction();
						break;
				}
			}
			catch (Exception e)
			{
				Log($"OnReply threw: {HKOracle.DescribeException(e)}");
			}
		}

		private static void SendNextAction()
		{
			var src = _steps[_next];
			// Fresh array per step: ActionDecoder.ApplyAction mutates action_vec in
			// place for hard-commit holds (oracle/Game/ProxyController.cs).
			Enqueue(new Message
			{
				type = "action",
				data = new MessageData { action_vec = new[] { src[0], src[1], src[2], src[3] } }
			});
		}

		private static void Finish(string why)
		{
			if (_finished) return;
			_finished = true;
			Log($"finishing ({why}) after {_next} steps — sending close");
			Enqueue(new Message { type = "close", data = new MessageData() });
			if (_mb != null) _mb.StartCoroutine(QuitSoon());
			else { TraceRecorder.CloseTrace(); Application.Quit(); }
		}

		private static IEnumerator QuitSoon()
		{
			// Let the runtime loop dequeue `close`, run Dispose(), and unwind.
			for (int i = 0; i < 8; i++) yield return null;
			TraceRecorder.CloseTrace();
			Log("quit");
			Application.Quit();
		}

		private static string Sha256Hex(string text)
		{
			using (var sha = SHA256.Create())
			{
				byte[] h = sha.ComputeHash(Encoding.UTF8.GetBytes(text));
				var sb = new StringBuilder(64);
				foreach (byte b in h) sb.Append(b.ToString("x2", CultureInfo.InvariantCulture));
				return sb.ToString();
			}
		}

		private sealed class DriverBehaviour : MonoBehaviour { }
	}
}
