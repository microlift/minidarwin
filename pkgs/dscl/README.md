# dscl and dseditgroup

`/usr/bin/dscl` and `/usr/sbin/dseditgroup` for the local directory node,
which they keep as macOS does: one XML property list per record under
`/private/var/db/dslocal/nodes/Default/users` and `.../groups`, each a
dictionary of attribute names to arrays of strings. Package scripts that
create service accounts, such as MacPorts' `postinstall`, use them.

The node is not part of the rootfs. A rootfs's system files are restored by
repair, and accounts must survive that, so the directories are made on the
first write.

## dscl

```
dscl [-q] DATASOURCE COMMAND PATH [ARGS...]
```

`DATASOURCE` is `.` or `/Local/Default`, or `/Search`, which here searches
the same records. `PATH` is `/Users`, `/Groups`, or a record under one.
Commands, with or without their dash:

- `-read PATH [KEY...]`, `-readall PATH [KEY...]`
- `-list PATH [KEY]`: a record type's record names, with `KEY`'s values; of
  a record, nothing, failing if it does not exist
- `-search PATH KEY VALUE`
- `-create PATH [KEY [VALUE...]]`, `-append PATH KEY VALUE...`
- `-delete PATH [KEY [VALUE...]]`, `-change PATH KEY OLD NEW`

Standard attribute names (`UniqueID`, `PrimaryGroupID`, `NFSHomeDirectory`,
`UserShell`, `RealName`, `Password`, `GeneratedUID`, `GroupMembership`, ...)
are stored under dslocal's own keys (`uid`, `gid`, `home`, ...);
`dsAttrTypeNative:KEY`, or any other name, is stored as given. A new record
gets a random `GeneratedUID`. Records cannot be renamed, `-passwd` and the
other commands are not provided, and only XML property lists are read.

A record the node does not hold is answered from the system's own account
database (`getpwnam`, `getgrnam` and their id forms), as a read-only view: a
change copies it into the node first, and it cannot be deleted. `-search`
reaches those records only by name and id, and `-list` of a record type
lists the node's own. Under Rosemary that database is the prefix's root and
admin accounts and the host's users, and Rosemary reads the node's records
in turn, so an account made here is seen by `getpwnam`. MiniDarwin's own
Libinfo does not read the node yet.

Errors are reported as Directory Services reports them, for example
`<dscl_cmd> DS Error: -14136 (eDSRecordNotFound)`, with exit status 1.
Changing the node needs root.

## dseditgroup

```
dseditgroup [-q] [-n NODE] -o create [-i GID] [-r REALNAME] GROUP
dseditgroup [-q] [-n NODE] -o delete GROUP
dseditgroup [-q] [-n NODE] -o edit -a USER|-d USER -t user GROUP
```

A group created without `-i` gets the lowest id from 501 that neither the
node nor the system's group database uses. `-u` and `-P` (credentials for a
remote node) are accepted and ignored; other operations, including `read`
and `checkmember`, are refused.

## Tests

`nix flake check` runs `scripts/test_dscl.py` against the host build
(`dsclBootstrap`), with `DSCL_TEST_NODE` naming a temporary node directory.
That variable also lets a test that is not root write; it is not for any
other use.
