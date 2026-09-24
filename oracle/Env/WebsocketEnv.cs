using System.Collections;
using System.Collections.Concurrent;
using System.Collections.Generic;
using WebSocketSharp;
using Newtonsoft.Json;
using UnityEngine;

namespace HKOracle.Env
{
	public class Socket : WebSocket
	{
		// ConcurrentQueue: OnMessage enqueues on websocket-sharp's thread while the
		// Unity main thread dequeues. Arrived is signalled after each enqueue so the
		// main thread can block on the next request (WebsocketEnv.NextMessage).
		public ConcurrentQueue<Message> UnreadMessages { get; private set; } = new ConcurrentQueue<Message>();
		public readonly System.Threading.AutoResetEvent Arrived = new System.Threading.AutoResetEvent(false);
		public Message LastMessageSent { get; private set; }
		// When set (script and dump mode), outgoing messages go here instead of the
		// wire and Connect() is skipped. The driver feeds inbound messages via
		// UnreadMessages from the main thread.
		public static System.Action<Message> Sink;

		public Socket(string url, params string[] protocols) : base(url, protocols)
		{
			this.OnMessage += (sender, e) =>
			{
				Message m;
				if (e.IsBinary)
					m = BinaryProtocol.Unpack(e.RawData);
				else
					m = JsonConvert.DeserializeObject<Message>(e.Data);
				UnreadMessages.Enqueue(m);
				Arrived.Set();
			};
		}

		public void Send(Message data)
		{
			data.sender = "client";
			LastMessageSent = data;
			if (Sink != null) { Sink(data); return; }
			byte[] binaryData = BinaryProtocol.Pack(data);
			base.Send(binaryData);
		}
	}

	// The request pump. Every agent step is exactly one frozen frame plus frames_per_wait
	// live frames, whatever the policy's latency (docs/frame-order.md "One agent step"):
	// after a reply that carries an observation (reset / step), the next request is
	// served in the frame after it, and the main thread blocks inside that frame until
	// the request arrives. Waiting by yielding frames instead would run one extra frozen
	// frame (Update, LateUpdate, pending Starts, coroutine resumes) per frame of latency.
	public abstract class WebsocketEnv
	{
		public Socket socket;
		protected bool _terminate = false;
		// Time.frameCount of the last reply that carried an observation.
		private int _obsFrame = -1;
		// Requests served in a frame other than _obsFrame + 1 (none unless a request
		// handler yields before its first frame): logged, and counted per process.
		public int LateServes { get; private set; }

		private const int kWaitSliceMs = 1000;
		private const int kWaitLogEveryMs = 30000;
		// While blocked this long without a request, the peer must answer a websocket ping
		// (the trainer's asyncio server pongs from its IO thread, busy or not), or the env ends:
		// a peer that vanished without a TCP close would otherwise hold the main thread forever.
		private const int kPingEveryMs = 60000;

		public WebsocketEnv(string url, params string[] protocols)
		{
			socket = new Socket(url, protocols);
		}

		protected void Connect()
		{
			if (Socket.Sink != null) return;
			socket.Connect();
		}

		protected abstract IEnumerator Setup();
		protected abstract IEnumerator Dispose();
		protected abstract IEnumerator OnMessage(Message message);
		// A request that needs no frame (pause, resume, close): handled synchronously
		// inside the frame it is read in. Returns false for reset / action.
		protected abstract bool HandleNow(Message message);

		// The frame the current request is served in, relative to the last observation.
		protected int FramesSinceObs => _obsFrame < 0 ? -1 : Time.frameCount - _obsFrame;

		// Blocks the main thread (no frame advances) until a request is queued. A driver
		// in this process (Socket.Sink) enqueues from the main thread itself, so with a
		// Sink an empty queue yields a frame instead: the driver has finished. Ends the env
		// when the socket closes or the peer stops answering pings (kPingEveryMs).
		private bool TryTakeBlocking(out Message message)
		{
			int waitedMs = 0;
			while (!socket.UnreadMessages.TryDequeue(out message))
			{
				if (Socket.Sink != null || _terminate) return false;
				if (socket.ReadyState == WebSocketState.Closed || socket.ReadyState == WebSocketState.Closing)
				{
					HKOracle.Instance.Log("[Pump] socket closed while waiting for a request");
					_terminate = true;
					return false;
				}
				socket.Arrived.WaitOne(kWaitSliceMs);
				waitedMs += kWaitSliceMs;
				if (waitedMs % kWaitLogEveryMs == 0)
					HKOracle.Instance.Log($"[Pump] blocked {waitedMs / 1000}s waiting for the next request (no frame runs)");
				if (waitedMs % kPingEveryMs == 0 && socket.UnreadMessages.IsEmpty && !socket.Ping())
				{
					HKOracle.Instance.Log($"[Pump] peer did not answer a ping after {waitedMs / 1000}s without a request; ending the env");
					_terminate = true;
					return false;
				}
			}
			return true;
		}

		private IEnumerator _runtime()
		{
			yield return Setup();
			while (!_terminate)
			{
				// The frozen frame of the next step is the frame after the observation.
				while (_obsFrame >= 0 && Time.frameCount <= _obsFrame) yield return null;
				if (!TryTakeBlocking(out var message))
				{
					if (_terminate) break;
					yield return null;
					continue;
				}
				if (HandleNow(message)) continue;
				// Coalesce a reset backlog: the trainer's watchdog re-sends reset while
				// one is stuck but awaits a single reply, and replies are matched by
				// order, so answering stale resets would desync the wire.
				if (message.type == "reset")
				{
					int stale = 0;
					while (socket.UnreadMessages.TryPeek(out var nxt)
						&& nxt.type == "reset")
					{
						socket.UnreadMessages.TryDequeue(out message);
						stale++;
					}
					if (stale > 0)
						HKOracle.Instance.Log(
							$"[Pump] coalesced {stale} stale queued reset(s)");
				}
				else if (FramesSinceObs != 1)
				{
					LateServes++;
					HKOracle.Instance.Log($"[Pump] {message.type} served {FramesSinceObs} frames after the observation (want 1); late serves {LateServes}");
				}
				yield return OnMessage(message);
			}
			yield return Dispose();
		}

		protected void SendMessage(Message message)
		{
			message.sender = "client";
			if (message.type == "reset" || message.type == "step") _obsFrame = Time.frameCount;
			socket.Send(message);
		}

		// Setup's wait for `init`, before any episode: frames may pass.
		protected IEnumerator WaitForInit()
		{
			while (socket.UnreadMessages.IsEmpty) yield return null;
		}

		public void Start()
		{
			GameManager.instance.StartCoroutine(_runtime());
		}

		protected void CloseSocket()
		{
			socket.Close();
		}

		public void Close()
		{
			_terminate = true;
		}
	}
}
