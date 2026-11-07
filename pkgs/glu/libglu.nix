# Mesa's GLU 9.0.3: /usr/lib/libGLU.1.dylib (+ libGLU.dylib) and <GL/glu.h>.
# Apple's GLU is closed (OpenGL.framework's libGLU.dylib); this is Mesa's
# standalone release, built as its meson.build does: the one `GLU` library
# target, -DLIBRARYBUILD, hidden visibility (exports are what <GL/gl.h>'s
# GLAPI marks default), darwin_versions 5.0.0/5.1.0, installed under /usr/lib
# as meson names it with prefix /usr.
#
# meson.build's gl_provider dependency is not built -- there is no libGL
# here, Apple's being closed. <GL/gl.h> comes from Mesa's own tarball,
# headers only (glHeaders, never the SDK), and every gl* call is a declared
# hole labelled `OpenGL`, linked -Wl,-U with no load command: whatever loads
# GLU supplies GL. As upstream, only glu.h is installed, not gl.h.
{ lib, mkDarwinPackage, runCommand, sources, toolchain }:

let
  version = "9.0.3";

  glHeaders = runCommand "mesa-gl-headers" { } ''
    tar -xf ${sources.mesa} --strip-components=1 \
      --wildcards '*/include/GL/gl.h' '*/include/GL/glext.h' '*/include/KHR/khrplatform.h'
    mkdir -p $out
    cp -R include/GL include/KHR $out/
  '';

  # src/meson.build's library('GLU', ...), in its order.
  libutil = [ "error" "glue" "mipmap" "project" "quad" "registry" ];
  libtess = [ "dict" "geom" "memalloc" "mesh" "normal" "priorityq" "render" "sweep" "tess" "tessmono" ];
  interface = [
    "bezierEval" "bezierPatch" "bezierPatchMesh" "glcurveval" "glinterface"
    "glrenderer" "glsurfeval" "incurveeval" "insurfeval"
  ];
  internals = [
    "arc" "arcsorter" "arctess" "backend" "basiccrveval" "basicsurfeval" "bin"
    "bufpool" "cachingeval" "ccw" "coveandtiler" "curve" "curvelist" "curvesub"
    "dataTransform" "displaylist" "flist" "flistsorter" "hull" "intersect"
    "knotvector" "mapdesc" "mapdescv" "maplist" "mesher"
    "monoTriangulationBackend" "monotonizer" "mycode" "nurbsinterfac"
    "nurbstess" "patch" "patchlist" "quilt" "reader" "renderhints" "slicer"
    "sorter" "splitarcs" "subdivider" "tobezier" "trimline" "trimregion"
    "trimvertpool" "uarray" "varray"
  ];
  nurbtess = [
    "directedLine" "gridWrap" "monoChain" "monoPolyPart" "monoTriangulation"
    "partitionX" "partitionY" "polyDBG" "polyUtil" "primitiveStream"
    "quicksort" "rectBlock" "sampleComp" "sampleCompBot" "sampleCompRight"
    "sampleCompTop" "sampleMonoPoly" "sampledLine" "searchTree"
  ];
  cSrcs = map (f: "src/libutil/${f}.c") libutil
    ++ map (f: "src/libtess/${f}.c") libtess;
  cxxSrcs = map (f: "src/libnurbs/interface/${f}.cc") interface
    ++ map (f: "src/libnurbs/internals/${f}.cc") internals
    ++ map (f: "src/libnurbs/nurbtess/${f}.cc") nurbtess;

  # OpenGL itself (closed, never built), and libm (absent-members.nix).
  allowUndefined = lib.genAttrs (map (s: "_gl${s}") [
    "Begin" "Color3f" "Disable" "Enable" "End" "EvalCoord1f" "EvalCoord2f"
    "EvalMesh1" "EvalMesh2" "EvalPoint1" "EvalPoint2" "GetFloatv"
    "GetIntegerv" "GetString" "GetTexLevelParameteriv" "Map1f" "Map2f"
    "MapGrid1f" "MapGrid2d" "MapGrid2f" "MultMatrixd" "MultMatrixf"
    "Normal3f" "Normal3fv" "Ortho" "PixelStorei" "PolygonMode" "PopAttrib"
    "PushAttrib" "Scalef" "TexCoord2f" "TexImage1D" "TexImage2D" "TexImage3D"
    "Translated" "Translatef" "Vertex2f" "Vertex2fv" "Vertex3f" "Vertex3fv"
  ]) (_: "OpenGL")
  // lib.genAttrs [ "_cos" "_sin" "___sincos_stret" ] (_: "system_m");
in
mkDarwinPackage {
  pname = "libglu";
  inherit version;
  src = sources.glu;
  inherit toolchain;

  passthru = {
    inherit allowUndefined glHeaders;
    installName = "/usr/lib/libGLU.1.dylib";
  };

  buildPhase = ''
    runHook preBuild
    export MD_SRCROOT=$PWD
    # meson's release defaults (b_ndebug=if-release) and the target's flags.
    common=(-O2 -DNDEBUG -DLIBRARYBUILD -fvisibility=hidden
      -I"$PWD/include" -I"$PWD/src/include"
      -I"$PWD/src/libnurbs/internals" -I"$PWD/src/libnurbs/interface"
      -I"$PWD/src/libnurbs/nurbtess" -I${glHeaders})
    md_compile "$PWD/obj" "$CC" "''${common[@]}" \
      -- ${lib.concatMapStringsSep " " (f: "$PWD/${f}") cSrcs}
    md_compile "$PWD/obj" "$CXX" "''${common[@]}" -fvisibility-inlines-hidden \
      -- ${lib.concatMapStringsSep " " (f: "$PWD/${f}") cxxSrcs}

    # Linked as mk-cmds links C++: -no_implicit_dylibs keeps libc++abi
    # (re-exported by libc++.1.dylib) from getting a load command.
    objs=()
    while IFS= read -r o; do objs+=( "$o" ); done < <(find obj -name '*.o' | sort)
    "$CXX" -dynamiclib -Wl,-no_implicit_dylibs \
      -install_name /usr/lib/libGLU.1.dylib \
      -compatibility_version 5.0.0 -current_version 5.1.0 \
      -o libGLU.1.dylib "''${objs[@]}" \
      ${lib.concatMapStringsSep " " (s: "-Wl,-U,${s}") (lib.attrNames allowUndefined)}
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 libGLU.1.dylib "$out/usr/lib/libGLU.1.dylib"
    ln -s libGLU.1.dylib "$out/usr/lib/libGLU.dylib"
    install -Dm644 include/GL/glu.h "$out/usr/include/GL/glu.h"

    f="$out/usr/lib/libGLU.1.dylib"
    md_verify_pure "$f"
    md_verify_signed "$f"
    deps=$("$OTOOL" -L "$f" | tail -n +3 | awk '{ print $1 }' | sort | tr '\n' ' ')
    if [ "$deps" != "/usr/lib/libSystem.B.dylib /usr/lib/libc++.1.dylib " ]; then
      echo "libGLU: unexpected load commands: $deps" >&2; exit 1
    fi
    # The link fails on an undeclared hole; this fails on a stale one.
    stale=$(comm -23 \
      <(printf '%s\n' ${lib.escapeShellArgs (lib.attrNames allowUndefined)} | sort) \
      <("$NM" -u "$f" | awk '{ print $NF }' | sort))
    if [ -n "$stale" ]; then
      echo "libGLU: declared but not imported:" $stale >&2; exit 1
    fi
    md_verify_symbols "$f" \
      _gluBeginCurve _gluBuild2DMipmaps _gluErrorString _gluGetString \
      _gluLookAt _gluNewTess _gluPerspective _gluProject _gluTessVertex \
      _gluUnProject _gluNewNurbsRenderer _gluSphere
    # Only the glu* API is exported (hidden visibility, as upstream).
    leaked=$("$NM" -gU "$f" | awk '{ print $NF }' | grep -v '^_glu' || true)
    if [ -n "$leaked" ]; then
      echo "libGLU: exports beyond glu*: $leaked" >&2; exit 1
    fi
    runHook postInstall
  '';

  meta = {
    description = "Mesa OpenGL Utility library (GLU)";
    homepage = "https://gitlab.freedesktop.org/mesa/glu";
    license = with lib.licenses; [ sgi-b-20 mit ];
  };
}
