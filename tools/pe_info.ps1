# Minimal read-only PE inspector: machine, sections, imported DLLs (+ optional function names).
param([Parameter(Mandatory)][string]$Path, [switch]$Funcs, [string]$Match = '')
$b = [IO.File]::ReadAllBytes($Path)
function U16($o){[BitConverter]::ToUInt16($b,$o)} ; function U32($o){[BitConverter]::ToUInt32($b,$o)}
function U64($o){[BitConverter]::ToUInt64($b,$o)}
function CStr($o){$e=$o; while($b[$e]){$e++}; [Text.Encoding]::ASCII.GetString($b,$o,$e-$o)}
$pe = U32 0x3c; $mach = U16 ($pe+4); $nsec = U16 ($pe+6); $optsz = U16 ($pe+20); $opt = $pe+24
$is64 = (U16 $opt) -eq 0x20b
$ddir = if($is64){$opt+112}else{$opt+96}
$secs = @(); $s = $opt+$optsz
for($i=0;$i -lt $nsec;$i++){ $o=$s+40*$i
  $secs += [pscustomobject]@{Name=[Text.Encoding]::ASCII.GetString($b,$o,8).TrimEnd([char]0); VA=U32($o+12); VSize=U32($o+8); Raw=U32($o+20); RawSize=U32($o+16); Chars=('{0:X8}' -f (U32($o+36)))} }
function R2O($rva){ foreach($x in $secs){ if($rva -ge $x.VA -and $rva -lt $x.VA+[Math]::Max($x.VSize,$x.RawSize)){ return $rva-$x.VA+$x.Raw } }; return -1 }
"machine=0x{0:X} pe32+={1} entry=0x{2:X} imagebase=0x{3:X}" -f $mach,$is64,(U32($opt+16)),$(if($is64){U64($opt+24)}else{U32($opt+28)})
$secs | Format-Table -AutoSize | Out-String -Width 200
$imp = U32 ($ddir+8); $o = R2O $imp
while($o -ge 0 -and (U32($o+12)) -ne 0){
  $dll = CStr (R2O (U32($o+12))); $thunk = U32 $o; if(!$thunk){$thunk=U32($o+16)}
  $names=@(); $t = R2O $thunk
  while($true){ if($is64){$v=U64 $t; $t+=8; if(!$v){break}; if($v -band 0x8000000000000000){$names+="#$($v -band 0xffff)";continue}}
    else{$v=U32 $t; $t+=4; if(!$v){break}; if($v -band 0x80000000){$names+="#$($v -band 0xffff)";continue}}
    $names += CStr ((R2O ([uint32]$v))+2) }
  "{0} ({1})" -f $dll,$names.Count
  if($Funcs){ $names | Where-Object { $_ -match $Match } | ForEach-Object { "    $_" } }
  $o += 20 }
