# The tool shims in macOS 27.2's /usr/bin: the hardlinks of
# tool-shim-public and of xtool-shim-public (tools that need Xcode, not
# just the Command Line Tools). Here each is a symlink to its shim.
{
  public = [
    "DeRez" "GetFileInfo" "ResMerger" "Rez" "SetFile" "SplitForks"
    "ar" "as" "asa" "bison" "bm4" "c++" "c++filt" "c89" "c99" "cc"
    "clang" "clang++" "clangd" "cmpdylib" "codesign_allocate" "cpp" "ctags"
    "ctf_insert" "dsymutil" "dwarfdump" "dyld_info" "flex" "flex++" "g++"
    "gatherheaderdoc" "gcc" "gcov" "git" "git-receive-pack" "git-shell"
    "git-upload-archive" "git-upload-pack" "gm4" "gnumake" "gperf"
    "hdxml2manxml" "headerdoc2html" "indent" "install_name_tool" "ld" "lex"
    "libtool" "lipo" "lldb" "llvm-g++" "llvm-gcc" "lorder" "m4" "make" "mig"
    "nm" "nmedit" "objdump" "otool" "pagestuff" "pip3" "python3" "ranlib"
    "resolveLinks" "rpcgen" "segedit" "size" "sourcekit-lsp" "strings"
    "strip" "swift" "swiftc" "unifdef" "unifdefall" "vtool" "xml2man" "yacc"
  ];
  requireXcode = [
    "actool" "agvtool" "desdp" "genstrings" "ibtool" "ictool" "opendiff"
    "sdef" "sdp" "stapler" "xcdebug" "xcodebuild" "xcscontrol" "xcsdiagnose"
    "xctrace" "xed"
  ];
}
