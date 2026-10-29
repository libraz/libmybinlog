#!/usr/bin/env bash
# Captures the version x configuration fixture matrix (see README.md).
#
# For every configuration listed in tests/models/capture_matrix_<flavor>.json
# this starts a throwaway server container, runs workload_<flavor>.sql block by
# block, snapshots every changed table after each data transaction with a
# server-side normalizing SELECT, and stores the binlog file(s) holding the
# workload together with expected.jsonl and meta.json. Expected values come
# from the server only. One container runs at a time.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
MODELS_DIR="$(cd "$HERE/../../models" && pwd)"
OUT_ROOT="$HERE"
DB=mx

usage() {
  cat <<'EOF'
Usage: capture_matrix.sh [options]
  --flavor mysql|mariadb   only this flavor (default: both)
  --version V              only this server version (e.g. 8.4, 11.8)
  --config NAME            only this config name (e.g. crc32_full_metafull_nozstd_nopjson)
  --force                  recapture configs whose meta.json already exists
  --out DIR                output root (default: the directory of this script)
  --list                   print the selected configs and exit
  -h, --help               this text
EOF
}

FLAVORS=(mysql mariadb)
SEL_VERSION=""
SEL_CONFIG=""
FORCE=0
LIST_ONLY=0
while (($#)); do
  case "$1" in
    --flavor) FLAVORS=("$2"); shift 2 ;;
    --version) SEL_VERSION="$2"; shift 2 ;;
    --config) SEL_CONFIG="$2"; shift 2 ;;
    --force) FORCE=1; shift ;;
    --out) OUT_ROOT="$(mkdir -p "$2" && cd "$2" && pwd)"; shift 2 ;;
    --list) LIST_ONLY=1; shift ;;
    -h | --help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

die() {
  echo "capture_matrix: $*" >&2
  exit 1
}

# Prints "version<TAB>name<TAB>key=value,key=value" per config of a model.
list_configs() {
  local model="$MODELS_DIR/capture_matrix_$1.json"
  [[ -f "$model" ]] || die "missing model $model"
  if command -v jq >/dev/null 2>&1; then
    jq -r '.configs[] | [.version, .name,
      (.settings | to_entries | map("\(.key)=\(.value)") | join(","))] | @tsv' "$model"
  else
    python3 -I -c '
import json, sys
for c in json.load(open(sys.argv[1]))["configs"]:
    s = ",".join(f"{k}={v}" for k, v in c["settings"].items())
    print(f"{c[\"version\"]}\t{c[\"name\"]}\t{s}")' "$model"
  fi
}

# setting <key> <k=v,k=v...>
setting() {
  local kv
  local IFS=,
  for kv in $2; do
    if [[ "${kv%%=*}" == "$1" ]]; then
      printf '%s' "${kv#*=}"
      return 0
    fi
  done
  die "setting $1 missing from '$2'"
}

# ver_ge A B: true when version A >= version B (numeric per component).
ver_ge() {
  local IFS=.
  local -a a=($1) b=($2)
  local i x y
  for i in 0 1 2; do
    x=$((10#${a[i]:-0}))
    y=$((10#${b[i]:-0}))
    ((x > y)) && return 0
    ((x < y)) && return 1
  done
  return 0
}

# Block arrays filled by parse_workload.
BLK_KIND=()
BLK_NAME=()
BLK_MIN=()
BLK_MAX=()
BLK_GTID=()

# parse_workload <file> <blockdir>: splits the workload into blocks at
# "-- @txn <name>" / "-- @ddl <name>" headers. Directive lines directly below a
# header gate or decorate that block; every other line is SQL body.
parse_workload() {
  local file="$1" dir="$2" line n=-1 in_body=0 lineno=0 names=" "
  local re_head='^--[[:space:]]*@(txn|ddl)[[:space:]]+([a-z0-9_]+)[[:space:]]*$'
  local re_dir='^--[[:space:]]*@(min-version|max-version|gtid-next)[[:space:]]+([^[:space:]]+)[[:space:]]*$'
  while IFS= read -r line || [[ -n "$line" ]]; do
    lineno=$((lineno + 1))
    if [[ "$line" =~ $re_head ]]; then
      n=$((n + 1))
      BLK_KIND[n]="${BASH_REMATCH[1]}"
      BLK_NAME[n]="${BASH_REMATCH[2]}"
      BLK_MIN[n]=""
      BLK_MAX[n]=""
      BLK_GTID[n]=""
      [[ "$names" != *" ${BLK_NAME[n]} "* ]] || die "$file:$lineno: duplicate block name ${BLK_NAME[n]}"
      names+="${BLK_NAME[n]} "
      : >"$dir/$n.sql"
      in_body=0
    elif [[ "$line" =~ $re_dir ]]; then
      ((n >= 0)) || die "$file:$lineno: directive before the first block header"
      ((in_body == 0)) || die "$file:$lineno: directive after SQL body"
      case "${BASH_REMATCH[1]}" in
        min-version) BLK_MIN[n]="${BASH_REMATCH[2]}" ;;
        max-version) BLK_MAX[n]="${BASH_REMATCH[2]}" ;;
        gtid-next)
          [[ "${BLK_KIND[n]}" == txn ]] || die "$file:$lineno: @gtid-next on a ddl block"
          BLK_GTID[n]="${BASH_REMATCH[2]}"
          ;;
      esac
    elif [[ "$line" =~ ^--[[:space:]]*@ ]]; then
      die "$file:$lineno: unknown directive: $line"
    elif ((n < 0)); then
      [[ -z "${line//[[:space:]]/}" || "$line" =~ ^-- ]] || die "$file:$lineno: SQL before the first block header"
    else
      if [[ -n "${line//[[:space:]]/}" && ! "$line" =~ ^-- ]]; then
        in_body=1
      fi
      printf '%s\n' "$line" >>"$dir/$n.sql"
    fi
  done <"$file"
  ((n >= 0)) || die "$file: no blocks"
}

# Normalizing SELECT expression and its tag per column type. The tags are
# recorded in meta.json; README.md documents what each one means.
norm_for() {
  local col="\`$1\`" dt
  dt="$(printf '%s' "$2" | tr '[:upper:]' '[:lower:]')"
  case "$dt" in
    tinyint | smallint | mediumint | int | bigint) NORM_EXPR="$col"; NORM_TAG=int ;;
    decimal) NORM_EXPR="CAST($col AS CHAR)"; NORM_TAG=decimal_text ;;
    float | double) NORM_EXPR="$col"; NORM_TAG=float ;;
    bit) NORM_EXPR="CAST($col AS UNSIGNED)"; NORM_TAG=bit_uint ;;
    enum | set) NORM_EXPR="CAST($col AS UNSIGNED)"; NORM_TAG=ordinal ;;
    year) NORM_EXPR="($col+0)"; NORM_TAG=year_int ;;
    date | datetime | time) NORM_EXPR="CAST($col AS CHAR)"; NORM_TAG=temporal_text ;;
    timestamp) NORM_EXPR="CAST(UNIX_TIMESTAMP($col) AS CHAR)"; NORM_TAG=unix_ts_text ;;
    char | varchar | tinytext | text | mediumtext | longtext)
      NORM_EXPR="CAST($col AS CHAR)"; NORM_TAG=text ;;
    binary | varbinary | tinyblob | blob | mediumblob | longblob | vector | geometry | point | \
      linestring | polygon | multipoint | multilinestring | multipolygon | geometrycollection | geomcollection)
      NORM_EXPR="HEX($col)"; NORM_TAG=hex ;;
    json) NORM_EXPR="CAST($col AS CHAR)"; NORM_TAG=json_text ;;
    inet4 | inet6 | uuid) NORM_EXPR="CAST($col AS CHAR)"; NORM_TAG=canonical_text ;;
    *) die "no normalization for column $1 of type $2" ;;
  esac
}

capture() {
  local flavor="$1" version="$2" name="$3" settings="$4"
  local image="$flavor:$version"
  local outdir="$OUT_ROOT/$flavor-$version/$name"
  if [[ -f "$outdir/meta.json" && $FORCE -eq 0 ]]; then
    echo "skip $flavor-$version/$name (meta.json exists; --force to recapture)"
    return 0
  fi

  local checksum image_mode metadata
  checksum="$(setting binlog_checksum "$settings")"
  image_mode="$(setting binlog_row_image "$settings")"
  metadata="$(setting binlog_row_metadata "$settings")"
  local -a args=(--server-id=1 --log-bin=binlog --binlog-format=ROW
    "--binlog-checksum=$checksum" "--binlog-row-image=$image_mode" "--binlog-row-metadata=$metadata")
  local client env_pw want_settings settings_sql gtid_sql workload
  if [[ "$flavor" == mysql ]]; then
    local comp pjson
    comp="$(setting binlog_transaction_compression "$settings")"
    pjson="$(setting binlog_row_value_options "$settings")"
    args+=(--gtid-mode=ON --enforce-gtid-consistency=ON "--binlog-transaction-compression=$comp")
    [[ "$pjson" == EMPTY ]] || args+=("--binlog-row-value-options=$pjson")
    client=mysql
    env_pw=MYSQL_ALLOW_EMPTY_PASSWORD=1
    want_settings="$checksum|$image_mode|$metadata|$comp|$pjson"
    settings_sql="SELECT CONCAT_WS('|', @@binlog_checksum, @@binlog_row_image, @@binlog_row_metadata,
      IF(@@binlog_transaction_compression, 'ON', 'OFF'),
      IF(@@binlog_row_value_options = '', 'EMPTY', @@binlog_row_value_options)),
      JSON_OBJECT('binlog_checksum', @@binlog_checksum, 'binlog_row_image', @@binlog_row_image,
      'binlog_row_metadata', @@binlog_row_metadata, 'binlog_format', @@binlog_format,
      'binlog_transaction_compression', IF(@@binlog_transaction_compression, 'ON', 'OFF'),
      'binlog_transaction_compression_level_zstd', @@binlog_transaction_compression_level_zstd,
      'binlog_row_value_options', @@binlog_row_value_options,
      'binlog_row_event_max_size', @@binlog_row_event_max_size,
      'gtid_mode', @@gtid_mode, 'server_uuid', @@server_uuid)"
    gtid_sql="SELECT '@@gtid', REPLACE(@@global.gtid_executed, '\n', '');"
  else
    local lbc
    lbc="$(setting log_bin_compress "$settings")"
    args+=(--binlog-annotate-row-events=ON "--log-bin-compress=$lbc")
    [[ "$lbc" == OFF ]] || args+=(--log-bin-compress-min-len=10)
    client=mariadb
    env_pw=MARIADB_ALLOW_EMPTY_ROOT_PASSWORD=1
    want_settings="$checksum|$image_mode|$metadata|$lbc"
    settings_sql="SELECT CONCAT_WS('|', @@binlog_checksum, @@binlog_row_image, @@binlog_row_metadata,
      IF(@@log_bin_compress, 'ON', 'OFF')),
      JSON_OBJECT('binlog_checksum', @@binlog_checksum, 'binlog_row_image', @@binlog_row_image,
      'binlog_row_metadata', @@binlog_row_metadata, 'binlog_format', @@binlog_format,
      'log_bin_compress', IF(@@log_bin_compress, 'ON', 'OFF'),
      'log_bin_compress_min_len', @@log_bin_compress_min_len,
      'binlog_annotate_row_events', IF(@@binlog_annotate_row_events, 'ON', 'OFF'),
      'column_compression_threshold', @@column_compression_threshold,
      'server_id', @@server_id, 'gtid_domain_id', @@gtid_domain_id)"
    gtid_sql="SELECT '@@gtid', @@global.gtid_binlog_pos;"
  fi
  workload="$HERE/workload_$flavor.sql"
  [[ -f "$workload" ]] || die "missing $workload"

  local tmp cname="mybinlog_matrix_$$"
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/capture_matrix.XXXXXX")"
  # shellcheck disable=SC2064
  trap "docker rm -f '$cname' >/dev/null 2>&1 || true; rm -rf '$tmp'" EXIT
  mkdir -p "$tmp/blocks" "$tmp/prev" "$tmp/cur" "$tmp/schema" "$tmp/out"

  BLK_KIND=() BLK_NAME=() BLK_MIN=() BLK_MAX=() BLK_GTID=()
  parse_workload "$workload" "$tmp/blocks"

  echo "== $flavor-$version/$name: ${args[*]}"
  docker rm -f "$cname" >/dev/null 2>&1 || true
  docker run -d --name "$cname" -e "$env_pw" "$image" "${args[@]}" >/dev/null

  # The image init runs a temporary server with networking off; TCP answers
  # only once the final server is up.
  local tries=0
  until docker exec "$cname" "$client" -uroot -h127.0.0.1 --protocol=TCP -e 'SELECT 1' >/dev/null 2>&1; do
    tries=$((tries + 1))
    if ((tries > 180)); then
      docker logs "$cname" 2>&1 | tail -20 >&2
      die "server in $cname did not come up"
    fi
    sleep 1
  done

  local -a cargs=("$client" -uroot -h127.0.0.1 --protocol=TCP -N -B -r --default-character-set=utf8mb4)
  [[ "$flavor" == mysql ]] || cargs+=(--skip-ssl-verify-server-cert)
  local -a q=(docker exec "$cname" "${cargs[@]}")
  local -a qi=(docker exec -i "$cname" "${cargs[@]}")
  local prelude="SET NAMES utf8mb4; SET SESSION sql_mode=''; SET SESSION time_zone='+00:00';"

  local line got server_settings server_version
  line="$("${q[@]}" -e "$settings_sql")"
  got="${line%%$'\t'*}"
  server_settings="${line#*$'\t'}"
  [[ "$got" == "$want_settings" ]] || die "server settings '$got' differ from requested '$want_settings'"
  server_version="$("${q[@]}" -e 'SELECT VERSION()')"

  "${q[@]}" -e "CREATE DATABASE $DB CHARACTER SET utf8mb4"
  "${q[@]}" -e 'FLUSH BINARY LOGS'
  local first_file
  first_file="$("${q[@]}" -e 'SHOW BINARY LOGS' | tail -1 | cut -f1)"

  local expected="$tmp/out/expected.jsonl"
  : >"$expected"
  local tables=() txns="" skipped="" seq=0 i n=${#BLK_NAME[@]}
  local kind bname reason input t js gtid emitted

  for ((i = 0; i < n; i++)); do
    kind="${BLK_KIND[i]}"
    bname="${BLK_NAME[i]}"
    reason=""
    if [[ -n "${BLK_MIN[i]}" ]] && ! ver_ge "$version" "${BLK_MIN[i]}"; then
      reason="min-version ${BLK_MIN[i]}"
    elif [[ -n "${BLK_MAX[i]}" ]] && ! ver_ge "${BLK_MAX[i]}" "$version"; then
      reason="max-version ${BLK_MAX[i]}"
    fi
    if [[ -n "$reason" ]]; then
      skipped+="${skipped:+,}{\"name\":\"$bname\",\"kind\":\"$kind\",\"reason\":\"$reason\"}"
      continue
    fi
    seq=$((seq + 1))

    input="$tmp/in.sql"
    {
      printf '%s\nUSE %s;\n' "$prelude" "$DB"
      if [[ "$kind" == txn ]]; then
        [[ -z "${BLK_GTID[i]}" ]] || printf "SET gtid_next='%s';\n" "${BLK_GTID[i]}"
        printf 'START TRANSACTION;\n'
        cat "$tmp/blocks/$i.sql"
        printf 'COMMIT;\n'
        [[ -z "${BLK_GTID[i]}" ]] || printf "SET gtid_next='AUTOMATIC';\n"
        for t in "${tables[@]}"; do cat "$tmp/schema/$t.sel"; done
      else
        cat "$tmp/blocks/$i.sql"
      fi
      printf '%s\n' "$gtid_sql"
    } >"$input"
    "${qi[@]}" <"$input" >"$tmp/result" || die "block $bname failed on $image ($name)"

    gtid=""
    for t in "${tables[@]}"; do : >"$tmp/cur/$t"; done
    while IFS=$'\t' read -r t js; do
      if [[ "$t" == "@@gtid" ]]; then
        gtid="$js"
      elif [[ "$kind" == txn ]]; then
        if [[ -s "$tmp/cur/$t" ]]; then printf ',%s' "$js"; else printf '%s' "$js"; fi >>"$tmp/cur/$t"
      fi
    done <"$tmp/result"

    emitted=""
    if [[ "$kind" == txn ]]; then
      for t in "${tables[@]}"; do
        cmp -s "$tmp/cur/$t" "$tmp/prev/$t" && continue
        {
          printf '{"txn":"%s","seq":%d,"table":"%s","rows":[' "$bname" "$seq" "$t"
          cat "$tmp/cur/$t"
          printf ']}\n'
        } >>"$expected"
        mv "$tmp/cur/$t" "$tmp/prev/$t"
        emitted+="${emitted:+,}\"$t\""
      done
      [[ -n "$emitted" ]] || die "txn $bname changed no table; every txn block must change data"
    else
      refresh_schema
    fi
    txns+="${txns:+,}{\"seq\":$seq,\"name\":\"$bname\",\"kind\":\"$kind\""
    [[ -z "${BLK_GTID[i]}" ]] || txns+=",\"gtid_next\":\"${BLK_GTID[i]}\""
    txns+=",\"gtid_after\":\"$gtid\",\"tables\":[$emitted]}"
  done

  # A client that silently fell back to latin1 double-encodes every literal.
  local enc
  enc="$("${q[@]}" -e "SELECT CONCAT_WS(',', HEX(c_l), HEX(v_s), HEX(c_u3)) FROM $DB.t_str WHERE id = 5")"
  [[ "$enc" == "E9,F09D849E,C3A7C3A0" ]] || die "charset sanity check failed: t_str id=5 is '$enc'"

  "${q[@]}" -e 'FLUSH BINARY LOGS'
  local files=() f
  while IFS=$'\t' read -r f _; do
    [[ "$f" < "$first_file" ]] && continue
    files+=("$f")
  done < <("${q[@]}" -e 'SHOW BINARY LOGS')
  unset 'files[${#files[@]}-1]' # the file opened by the last flush
  ((${#files[@]} > 0)) || die "no binlog files captured"
  local files_json=""
  for f in "${files[@]}"; do
    docker cp "$cname:/var/lib/mysql/$f" "$tmp/out/$f"
    files_json+="${files_json:+,}\"$f\""
  done

  local tables_json="" flags_json="" a
  for t in "${tables[@]}"; do
    tables_json+="${tables_json:+,}{\"name\":\"$t\",\"primary_key\":[$(cat "$tmp/schema/$t.pk")],\"columns\":[$(cat "$tmp/schema/$t.cols")]}"
  done
  for a in "${args[@]}"; do flags_json+="${flags_json:+,}\"$a\""; done
  local settings_json="" kv
  local IFS_save="$IFS"
  IFS=,
  for kv in $settings; do settings_json+="${settings_json:+,}\"${kv%%=*}\":\"${kv#*=}\""; done
  IFS="$IFS_save"

  {
    printf '{"flavor":"%s","image":"%s","version":"%s","config":"%s",' "$flavor" "$image" "$version" "$name"
    printf '"server_version":"%s","settings":{%s},"server_settings":%s,' "$server_version" "$settings_json" "$server_settings"
    printf '"server_flags":[%s],"workload":"workload_%s.sql","database":"%s",' "$flags_json" "$flavor" "$DB"
    printf '"session":{"sql_mode":"","time_zone":"+00:00"},"binlog_files":[%s],' "$files_json"
    printf '"tables":[%s],"transactions":[%s],"skipped":[%s]}\n' "$tables_json" "$txns" "$skipped"
  } >"$tmp/meta.raw"

  # Validates both JSON outputs; meta.json is pretty-printed for review.
  if command -v jq >/dev/null 2>&1; then
    jq . "$tmp/meta.raw" >"$tmp/out/meta.json"
    jq -e . "$expected" >/dev/null
  else
    python3 -I -m json.tool "$tmp/meta.raw" >"$tmp/out/meta.json"
    python3 -I -c 'import json,sys
for l in open(sys.argv[1]): json.loads(l)' "$expected"
  fi

  local snapshot_lines
  snapshot_lines=$(wc -l <"$tmp/out/expected.jsonl" | tr -d ' ')
  # Binlogs and snapshots are stored gzip-compressed; tests read them through zlib.
  gzip -9n "$tmp/out/expected.jsonl" "${files[@]/#/$tmp/out/}"

  rm -rf "$outdir"
  mkdir -p "$(dirname "$outdir")"
  mv "$tmp/out" "$outdir"
  docker rm -f "$cname" >/dev/null
  rm -rf "$tmp"
  trap - EXIT
  echo "   $server_version -> $outdir (${files[*]}, $seq blocks, $snapshot_lines snapshot lines)"
}

# Rebuilds the per-table snapshot SELECTs and the meta.json column records
# from information_schema after a ddl block. Uses capture()'s locals.
refresh_schema() {
  local sql="SELECT c.TABLE_NAME, c.COLUMN_NAME, c.DATA_TYPE, IFNULL(s.SEQ_IN_INDEX, 0), c.IS_NULLABLE,
    JSON_OBJECT('name', c.COLUMN_NAME, 'data_type', c.DATA_TYPE, 'column_type', c.COLUMN_TYPE,
      'charset', c.CHARACTER_SET_NAME, 'nullable', c.IS_NULLABLE,
      'compressed', IF(c.COLUMN_TYPE LIKE '%COMPRESSED%', 'YES', 'NO'))
    FROM information_schema.COLUMNS c
    JOIN information_schema.TABLES t
      ON t.TABLE_SCHEMA = c.TABLE_SCHEMA AND t.TABLE_NAME = c.TABLE_NAME AND t.TABLE_TYPE = 'BASE TABLE'
    LEFT JOIN information_schema.STATISTICS s
      ON s.TABLE_SCHEMA = c.TABLE_SCHEMA AND s.TABLE_NAME = c.TABLE_NAME
      AND s.COLUMN_NAME = c.COLUMN_NAME AND s.INDEX_NAME = 'PRIMARY'
    WHERE c.TABLE_SCHEMA = '$DB' ORDER BY c.TABLE_NAME, c.ORDINAL_POSITION"
  local tn col dt pk nullable js cur_t="" exprs="" pks="" order="" cols=""
  tables=()
  rm -f "$tmp/schema/"*
  flush_table() {
    [[ -n "$cur_t" ]] || return 0
    [[ -n "$pks" ]] || die "table $cur_t has no primary key; replay needs one"
    printf "SELECT '%s', JSON_OBJECT(%s) FROM \`%s\`.\`%s\` ORDER BY %s;\n" \
      "$cur_t" "$exprs" "$DB" "$cur_t" "$order" >"$tmp/schema/$cur_t.sel"
    printf '%s' "$pks" >"$tmp/schema/$cur_t.pk"
    printf '%s' "$cols" >"$tmp/schema/$cur_t.cols"
    [[ -f "$tmp/prev/$cur_t" ]] || : >"$tmp/prev/$cur_t"
    tables+=("$cur_t")
  }
  local -a pk_by_seq
  while IFS=$'\t' read -r tn col dt pk nullable js; do
    if [[ "$tn" != "$cur_t" ]]; then
      if [[ -n "$cur_t" ]]; then
        pks="" order=""
        for p in "${pk_by_seq[@]}"; do
          pks+="${pks:+,}\"$p\""
          order+="${order:+, }\`$p\`"
        done
        flush_table
      fi
      cur_t="$tn" exprs="" cols="" pk_by_seq=()
    fi
    norm_for "$col" "$dt"
    exprs+="${exprs:+, }'$col', $NORM_EXPR"
    cols+="${cols:+,}{\"norm\":\"$NORM_TAG\",\"pk_seq\":$pk,${js#\{}"
    if ((pk > 0)); then
      pk_by_seq[pk - 1]="$col"
    elif [[ "$nullable" != YES ]]; then
      die "column $tn.$col is NOT NULL; non-key columns must be nullable with default NULL"
    fi
  done < <("${q[@]}" -e "$sql")
  if [[ -n "$cur_t" ]]; then
    pks="" order=""
    for p in "${pk_by_seq[@]}"; do
      pks+="${pks:+,}\"$p\""
      order+="${order:+, }\`$p\`"
    done
    flush_table
  fi
}

command -v docker >/dev/null 2>&1 || die "docker is required"
for flavor in "${FLAVORS[@]}"; do
  [[ "$flavor" == mysql || "$flavor" == mariadb ]] || die "unknown flavor $flavor"
  configs=()
  while IFS= read -r line; do configs+=("$line"); done < <(list_configs "$flavor")
  for line in "${configs[@]}"; do
    IFS=$'\t' read -r version name settings <<<"$line"
    [[ -z "$SEL_VERSION" || "$SEL_VERSION" == "$version" ]] || continue
    [[ -z "$SEL_CONFIG" || "$SEL_CONFIG" == "$name" ]] || continue
    if ((LIST_ONLY)); then
      printf '%s-%s/%s\t%s\n' "$flavor" "$version" "$name" "$settings"
    else
      capture "$flavor" "$version" "$name" "$settings"
    fi
  done
done
