/*
 * camhal-ioctl.js (v1.0) - Frida, Galaxy S10 on ANDROID (root + frida-server): log what Samsung's camera HAL asks the
 * kernel camera driver (fimc-is, V4L2 nodes /dev/video*) to do. Optional companion of camtrace-s10.sh
 * (notes/s10-camera.md, 2026-11-17). It only observes: every ioctl goes through unchanged.
 *
 *   adb shell su -c "/data/local/tmp/frida-server &"
 *   frida -U -n <camera provider process> -l camhal-ioctl.js -o camhal-ioctl.txt
 *     (find the process with:  adb shell su -c "ps -A | grep -i camera"  - usually
 *      android.hardware.camera.provider@2.x-service or vendor.samsung.hardware.camera.provider...)
 *   then open the Camera app, front camera, a few seconds, close it; Ctrl+C.
 *
 * Output: one line per ioctl on a /dev/video* or /dev/v4l-subdev* file: device, request (V4L2 name when known),
 * return value; for S_CTRL the control id and value; for S_FMT the pixel format and size; for S_INPUT the input.
 */
'use strict';

const V4L2 = {           // VIDIOC_* values printed by a C program from linux/videodev2.h (LP64: the same layouts on arm64)
  0x80685600: 'QUERYCAP', 0xc0d05604: 'G_FMT', 0xc0d05605: 'S_FMT', 0xc0145608: 'REQBUFS', 0xc0585609: 'QUERYBUF',
  0xc058560f: 'QBUF', 0xc0585611: 'DQBUF', 0x40045612: 'STREAMON', 0x40045613: 'STREAMOFF', 0xc0cc5615: 'G_PARM',
  0xc0cc5616: 'S_PARM', 0xc008561b: 'G_CTRL', 0xc008561c: 'S_CTRL', 0x80045626: 'G_INPUT', 0xc0045627: 'S_INPUT',
  0xc0205647: 'G_EXT_CTRLS', 0xc0205648: 'S_EXT_CTRLS', 0x4014563c: 'S_CROP', 0xc014563b: 'G_CROP'
};

const readlink = new NativeFunction(Module.getExportByName(null, 'readlink'), 'int', ['pointer', 'pointer', 'int']);
const pathBuf = Memory.alloc(256);
function fdPath(fd) {
  const n = readlink(Memory.allocUtf8String('/proc/self/fd/' + fd), pathBuf, 255);
  return n > 0 ? pathBuf.readUtf8String(n) : '?';
}
function fourcc(v) { return String.fromCharCode(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff); }

Interceptor.attach(Module.getExportByName(null, 'ioctl'), {
  onEnter(args) {
    this.fd = args[0].toInt32();
    this.req = args[1].toUInt32();
    this.arg = args[2];
    this.path = fdPath(this.fd);
    this.keep = this.path.indexOf('/dev/video') === 0 || this.path.indexOf('/dev/v4l-subdev') === 0;
    if (!this.keep) return;
    let extra = '';
    const name = V4L2[this.req] || ('0x' + this.req.toString(16));
    try {
      if (name === 'S_CTRL') extra = ' id 0x' + this.arg.readU32().toString(16) + ' value ' + this.arg.add(4).readS32();
      else if (name === 'S_INPUT') extra = ' input 0x' + this.arg.readU32().toString(16);
      else if (name === 'S_FMT') {      // struct v4l2_format: type (4) + pad, union at 8: width, height, pixelformat
        const w = this.arg.add(8).readU32(), h = this.arg.add(12).readU32(), pf = this.arg.add(16).readU32();
        extra = ' ' + w + 'x' + h + ' ' + fourcc(pf);
      }
    } catch (e) { extra = ' (unreadable arg)'; }
    this.line = Date.now() + ' ' + this.path + ' ' + name + extra;
  },
  onLeave(ret) {
    if (this.keep) console.log(this.line + ' -> ' + ret.toInt32());
  }
});
console.log('camhal-ioctl.js v1.0 attached to ' + Process.id);
