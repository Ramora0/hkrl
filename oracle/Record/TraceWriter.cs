using System;
using System.IO;
using System.Text;
using HKOracle.Env;

namespace HKOracle.Record
{
	// Binary sink for docs/trace-format.md v1. Every offset here is dictated by
	// that file. Little-endian (BinaryWriter is LE on every .NET/Mono target);
	// str16 = u16 UTF-8 byte length + bytes.
	//
	// Records emitted before WriteHeader go to an in-memory buffer and are
	// appended verbatim right after the header. The header carries `capture.scene`
	// and `capture.frames_per_wait`, neither of which is known until the first
	// SceneReady, and the doc allows exactly three record kinds before that point
	// (RESET_BEGIN / SCENE_LOADED / RNG_SEED). A crash before SceneReady therefore
	// leaves a 0-byte file, which is a loud failure rather than a wrong header.
	public sealed class TraceWriter
	{
		// v2: ENTITY carries f32 rot / rot_t.  Readers accept v1 and v2.
		public const uint SchemaVersion = 2;
		private static readonly byte[] Magic = { (byte)'H', (byte)'K', (byte)'T', (byte)'R' };

		private const int kFlushEveryRecords = 256;
		private const float kFlushEverySeconds = 2f;

		private FileStream _fs;
		private MemoryStream _pending;
		private BinaryWriter _w;
		private int _recordsSinceFlush;
		private float _lastFlushRealtime;

		public string Path { get; private set; }
		public bool IsOpen => _w != null;
		public bool HeaderWritten { get; private set; }
		public long RecordCount { get; private set; }

		public TraceWriter(string path)
		{
			Path = path;
			string dir = System.IO.Path.GetDirectoryName(path);
			if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
			_fs = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.Read, 1 << 16);
			_pending = new MemoryStream(1 << 14);
			_w = new BinaryWriter(_pending, new UTF8Encoding(false));
		}

		public void WriteHeader(byte[] jsonUtf8)
		{
			if (HeaderWritten || _fs == null) return;
			var hw = new BinaryWriter(_fs, new UTF8Encoding(false));
			hw.Write(Magic, 0, 4);
			hw.Write(SchemaVersion);
			hw.Write((uint)jsonUtf8.Length);
			hw.Write(jsonUtf8, 0, jsonUtf8.Length);
			hw.Flush();
			byte[] buffered = _pending.ToArray();
			if (buffered.Length > 0) _fs.Write(buffered, 0, buffered.Length);
			_pending = null;
			_w = new BinaryWriter(_fs, new UTF8Encoding(false));
			HeaderWritten = true;
			Flush();
		}

		public void U8(byte v) { _w.Write(v); }
		public void U8(bool v) { _w.Write(v ? (byte)1 : (byte)0); }
		public void U16(ushort v) { _w.Write(v); }
		public void U32(uint v) { _w.Write(v); }
		public void U64(ulong v) { _w.Write(v); }
		public void I32(int v) { _w.Write(v); }
		public void F32(float v) { _w.Write(v); }

		public void Bytes(byte[] b, int len) { _w.Write(b, 0, len); }

		public void Str16(string s)
		{
			if (string.IsNullOrEmpty(s)) { _w.Write((ushort)0); return; }
			byte[] b = Encoding.UTF8.GetBytes(s);
			int len = b.Length > 65535 ? 65535 : b.Length;
			_w.Write((ushort)len);
			_w.Write(b, 0, len);
		}

		// Call once per completed record. Periodic flushing so a kill -9 leaves a
		// readable prefix.
		public void EndRecord()
		{
			RecordCount++;
			if (++_recordsSinceFlush < kFlushEveryRecords)
			{
				float now = UnityEngine.Time.realtimeSinceStartup;
				if (now - _lastFlushRealtime < kFlushEverySeconds) return;
			}
			Flush();
		}

		public void Flush()
		{
			if (_w == null) return;
			try
			{
				_w.Flush();
				if (HeaderWritten && _fs != null) _fs.Flush(true);
			}
			catch { }
			_recordsSinceFlush = 0;
			_lastFlushRealtime = UnityEngine.Time.realtimeSinceStartup;
		}

		public void Close()
		{
			if (_w == null) return;
			try { Flush(); } catch { }
			try { _w.Close(); } catch { }
			try { _fs?.Close(); } catch { }
			_w = null;
			_fs = null;
			_pending = null;
		}
	}
}
