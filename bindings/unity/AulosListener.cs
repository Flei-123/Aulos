// Put this on the camera (or the player head). It owns the runtime: it creates
// the system, loads the banks, pushes the listener transform every frame and
// shuts everything down on quit. Exactly one of these per scene.
using UnityEngine;

namespace Aulos
{
    [DefaultExecutionOrder(-1000)]     // before every AulosSource
    [AddComponentMenu("Audio/Aulos Listener")]
    public class AulosListener : MonoBehaviour
    {
        [Header("System")]
        public uint sampleRate = 48000;
        public uint maxVoices = 64;

        [Tooltip("Folder that sample paths in the bank are relative to. " +
                 "Empty = StreamingAssets.")]
        public string assetRoot = "";

        [Tooltip("Banks to load on Awake, relative to StreamingAssets.")]
        public string[] banks = { "banks/game.json" };

        [Header("Buses")]
        [Range(0f, 1f)] public float master = 1f;
        [Range(0f, 1f)] public float sfx = 1f;
        [Range(0f, 1f)] public float music = 1f;

        [Header("Debug")]
        public bool logStats = false;

        private Vector3 _lastPos;
        private float _statTimer;

        void Awake()
        {
            string root = string.IsNullOrEmpty(assetRoot)
                ? Application.streamingAssetsPath
                : assetRoot;

            if (!AulosRuntime.Create(sampleRate, maxVoices, root))
                return;

            foreach (var b in banks)
            {
                if (string.IsNullOrEmpty(b)) continue;
                string path = System.IO.Path.IsPathRooted(b)
                    ? b
                    : System.IO.Path.Combine(Application.streamingAssetsPath, b);
                AulosRuntime.LoadBank(path);
            }

            _lastPos = transform.position;
            DontDestroyOnLoad(gameObject);
        }

        void Update()
        {
            if (!AulosRuntime.IsRunning) return;

            // velocity from the transform: good enough for doppler, and it
            // works for objects without a Rigidbody
            Vector3 pos = transform.position;
            Vector3 vel = Time.deltaTime > 0f ? (pos - _lastPos) / Time.deltaTime : Vector3.zero;
            _lastPos = pos;

            AulosRuntime.SetListener(pos, transform.forward, transform.up, vel);
            AulosRuntime.SetBusVolume("master", master);
            AulosRuntime.SetBusVolume("sfx", sfx);
            AulosRuntime.SetBusVolume("music", music);
            AulosRuntime.Update();

            if (logStats)
            {
                _statTimer += Time.deltaTime;
                if (_statTimer >= 1f)
                {
                    _statTimer = 0f;
                    var s = AulosRuntime.Stats();
                    Debug.Log($"[Aulos] voices {s.activeVoices}/{s.maxVoices}  " +
                              $"started {s.started}  stolen {s.stolen}  dropped {s.dropped}  " +
                              $"peak {s.peakLeft:F2}/{s.peakRight:F2}");
                }
            }
        }

        void OnApplicationPause(bool paused)
        {
            if (paused) AulosRuntime.StopAll(0.1f);
        }

        void OnDestroy()
        {
            AulosRuntime.Destroy();
        }
    }
}
