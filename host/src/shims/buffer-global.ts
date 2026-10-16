// Injected into the JavaScriptCore bundle: the shared Photon code uses the global `Buffer`.
import { Buffer } from "buffer";
(globalThis as { Buffer?: unknown }).Buffer = Buffer;
export { Buffer };
