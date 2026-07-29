// The Aulos counterpart to Unity's AudioSource: name an event, the component
// keeps its position (and optionally its parameters) in sync with the transform.
//
//   var engine = car.GetComponent<AulosSource>();
//   engine.SetParameter("rpm", currentRpm);
//   engine.Stop(0.3f);
//
// A stopped or stolen voice invalidates the handle. Every call checks that, so
// holding a dead AulosSource around is harmless.
using UnityEngine;

namespace Aulos
{
    [AddComponentMenu("Audio/Aulos Source")]
    public class AulosSource : MonoBehaviour
    {
        [Tooltip("Event name as declared in the bank.")]
        public string eventName = "";

        public bool playOnAwake = true;

        [Tooltip("Send the transform position to the runtime every frame. " +
                 "Turn off for sounds that never move: it saves a command.")]
        public bool followTransform = true;

        [Tooltip("Report the transform velocity so doppler works.")]
        public bool sendVelocity = true;

        [Tooltip("Stop the voice when the object is disabled or destroyed.")]
        public bool stopOnDisable = true;

        [Range(0f, 4f)] public float volume = 1f;
        [Range(0.1f, 4f)] public float pitch = 1f;

        private uint _instance;
        private Vector3 _lastPos;
        private float _lastVolume = -1f, _lastPitch = -1f;

        public bool IsPlaying => AulosRuntime.IsPlaying(_instance);
        public uint Instance => _instance;

        void OnEnable()
        {
            _lastPos = transform.position;
            if (playOnAwake) Play();
        }

        void OnDisable()
        {
            if (stopOnDisable) Stop(0.05f);
        }

        public void Play()
        {
            if (!AulosRuntime.IsRunning || string.IsNullOrEmpty(eventName)) return;

            if (!AulosRuntime.EventExists(eventName))
            {
                Debug.LogWarning($"[Aulos] event \"{eventName}\" is not in any loaded bank", this);
                return;
            }

            _instance = AulosRuntime.Play3D(eventName, transform.position);
            _lastVolume = _lastPitch = -1f;      // force a resend
        }

        /// <summary>Fire and forget copy - the right call for footsteps and hits.</summary>
        public void PlayOneShot()
        {
            if (AulosRuntime.IsRunning) AulosRuntime.Play3D(eventName, transform.position);
        }

        public void Stop(float fadeSeconds = 0.1f)
        {
            if (_instance != 0) AulosRuntime.Stop(_instance, fadeSeconds);
            _instance = 0;
        }

        public void SetParameter(string name, float value)
        {
            if (_instance != 0) AulosRuntime.SetParameter(_instance, name, value);
        }

        void Update()
        {
            if (_instance == 0 || !AulosRuntime.IsRunning) return;

            if (!AulosRuntime.IsPlaying(_instance))   // finished or stolen
            {
                _instance = 0;
                return;
            }

            if (followTransform)
            {
                Vector3 pos = transform.position;
                Vector3 vel = Vector3.zero;
                if (sendVelocity && Time.deltaTime > 0f)
                    vel = (pos - _lastPos) / Time.deltaTime;
                _lastPos = pos;
                AulosRuntime.SetPosition(_instance, pos, vel);
            }

            // only send when the inspector value actually changed
            if (!Mathf.Approximately(volume, _lastVolume))
            {
                AulosRuntime.SetVolume(_instance, volume);
                _lastVolume = volume;
            }
            if (!Mathf.Approximately(pitch, _lastPitch))
            {
                AulosRuntime.SetPitch(_instance, pitch);
                _lastPitch = pitch;
            }
        }
    }
}
