-- Type-exhaustive workload for capture_matrix.sh (MySQL).
-- Grammar and conventions: README.md in this directory. Every table has a
-- primary key and every other column is nullable with default NULL, so a
-- column missing from a MINIMAL write image replays as NULL.

-- @ddl create_tables
CREATE TABLE t_int (
  id INT NOT NULL PRIMARY KEY,
  ti TINYINT NULL, tu TINYINT UNSIGNED NULL,
  si SMALLINT NULL, su SMALLINT UNSIGNED NULL,
  mi MEDIUMINT NULL, mu MEDIUMINT UNSIGNED NULL,
  i INT NULL, iu INT UNSIGNED NULL,
  bi BIGINT NULL, bu BIGINT UNSIGNED NULL
);
CREATE TABLE t_num (
  id INT NOT NULL PRIMARY KEY,
  d65 DECIMAL(65,30) NULL, d10 DECIMAL(10,0) NULL, d55 DECIMAL(5,5) NULL,
  d102 DECIMAL(10,2) NULL, d189 DECIMAL(18,9) NULL, d42 DECIMAL(4,2) NULL,
  f FLOAT NULL, d DOUBLE NULL,
  b1 BIT(1) NULL, b10 BIT(10) NULL, b64 BIT(64) NULL
);
CREATE TABLE t_time (
  id INT NOT NULL PRIMARY KEY,
  d DATE NULL,
  dt0 DATETIME(0) NULL,
  dt1 DATETIME(1) NULL,
  dt2 DATETIME(2) NULL,
  dt3 DATETIME(3) NULL,
  dt4 DATETIME(4) NULL,
  dt5 DATETIME(5) NULL,
  dt6 DATETIME(6) NULL,
  ts0 TIMESTAMP(0) NULL DEFAULT NULL,
  ts1 TIMESTAMP(1) NULL DEFAULT NULL,
  ts2 TIMESTAMP(2) NULL DEFAULT NULL,
  ts3 TIMESTAMP(3) NULL DEFAULT NULL,
  ts4 TIMESTAMP(4) NULL DEFAULT NULL,
  ts5 TIMESTAMP(5) NULL DEFAULT NULL,
  ts6 TIMESTAMP(6) NULL DEFAULT NULL,
  tm0 TIME(0) NULL,
  tm1 TIME(1) NULL,
  tm2 TIME(2) NULL,
  tm3 TIME(3) NULL,
  tm4 TIME(4) NULL,
  tm5 TIME(5) NULL,
  tm6 TIME(6) NULL,
  y YEAR NULL
);
CREATE TABLE t_str (
  id INT NOT NULL PRIMARY KEY,
  c_l CHAR(10) CHARACTER SET latin1 NULL,
  v_l VARCHAR(20) CHARACTER SET latin1 NULL,
  c_a CHAR(3) CHARACTER SET ascii NULL,
  c_u3 CHAR(100) CHARACTER SET utf8mb3 NULL,
  c_mb CHAR(255) CHARACTER SET utf8mb4 NULL,
  v_s VARCHAR(10) CHARACTER SET utf8mb4 NULL,
  v_mb VARCHAR(300) CHARACTER SET utf8mb4 NULL,
  bn BINARY(8) NULL,
  vb VARBINARY(300) NULL
) DEFAULT CHARSET=utf8mb4;
CREATE TABLE t_lob (
  id INT NOT NULL PRIMARY KEY,
  n INT NULL,
  tt TINYTEXT NULL, tx TEXT NULL, mt MEDIUMTEXT NULL, lt LONGTEXT NULL,
  tb TINYBLOB NULL, bl BLOB NULL, mb MEDIUMBLOB NULL, lb LONGBLOB NULL
) DEFAULT CHARSET=utf8mb4;
CREATE TABLE t_big (
  id INT NOT NULL PRIMARY KEY,
  n INT NULL,
  mt MEDIUMTEXT NULL,
  j JSON NULL
) DEFAULT CHARSET=utf8mb4;
CREATE TABLE t_enum (
  id INT NOT NULL PRIMARY KEY,
  e1 ENUM('a','b','c') NULL,
  e2 ENUM('v000','v001','v002','v003','v004','v005','v006','v007','v008','v009','v010','v011','v012','v013','v014','v015','v016','v017','v018','v019','v020','v021','v022','v023','v024','v025','v026','v027','v028','v029','v030','v031','v032','v033','v034','v035','v036','v037','v038','v039','v040','v041','v042','v043','v044','v045','v046','v047','v048','v049','v050','v051','v052','v053','v054','v055','v056','v057','v058','v059','v060','v061','v062','v063','v064','v065','v066','v067','v068','v069','v070','v071','v072','v073','v074','v075','v076','v077','v078','v079','v080','v081','v082','v083','v084','v085','v086','v087','v088','v089','v090','v091','v092','v093','v094','v095','v096','v097','v098','v099','v100','v101','v102','v103','v104','v105','v106','v107','v108','v109','v110','v111','v112','v113','v114','v115','v116','v117','v118','v119','v120','v121','v122','v123','v124','v125','v126','v127','v128','v129','v130','v131','v132','v133','v134','v135','v136','v137','v138','v139','v140','v141','v142','v143','v144','v145','v146','v147','v148','v149','v150','v151','v152','v153','v154','v155','v156','v157','v158','v159','v160','v161','v162','v163','v164','v165','v166','v167','v168','v169','v170','v171','v172','v173','v174','v175','v176','v177','v178','v179','v180','v181','v182','v183','v184','v185','v186','v187','v188','v189','v190','v191','v192','v193','v194','v195','v196','v197','v198','v199','v200','v201','v202','v203','v204','v205','v206','v207','v208','v209','v210','v211','v212','v213','v214','v215','v216','v217','v218','v219','v220','v221','v222','v223','v224','v225','v226','v227','v228','v229','v230','v231','v232','v233','v234','v235','v236','v237','v238','v239','v240','v241','v242','v243','v244','v245','v246','v247','v248','v249','v250','v251','v252','v253','v254','v255','v256','v257','v258','v259','v260','v261','v262','v263','v264','v265','v266','v267','v268','v269','v270','v271','v272','v273','v274','v275','v276','v277','v278','v279','v280','v281','v282','v283','v284','v285','v286','v287','v288','v289','v290','v291','v292','v293','v294','v295','v296','v297','v298','v299') NULL,
  s1 SET('x','y','z') NULL,
  s2 SET('m00','m01','m02','m03','m04','m05','m06','m07','m08','m09','m10','m11') NULL,
  s8 SET('b00','b01','b02','b03','b04','b05','b06','b07','b08','b09','b10','b11','b12','b13','b14','b15','b16','b17','b18','b19','b20','b21','b22','b23','b24','b25','b26','b27','b28','b29','b30','b31','b32','b33','b34','b35','b36','b37','b38','b39','b40','b41','b42','b43','b44','b45','b46','b47','b48','b49','b50','b51','b52','b53','b54','b55','b56','b57','b58','b59','b60','b61','b62','b63') NULL
) DEFAULT CHARSET=utf8mb4;
CREATE TABLE t_json (
  id INT NOT NULL PRIMARY KEY,
  n INT NULL,
  j JSON NULL
) DEFAULT CHARSET=utf8mb4;
CREATE TABLE t_geo (
  id INT NOT NULL PRIMARY KEY,
  g GEOMETRY NULL,
  p POINT NULL,
  pg POLYGON NULL SRID 4326
);
CREATE TABLE t_bulk (
  id INT NOT NULL PRIMARY KEY,
  a INT NULL,
  s VARCHAR(16) NULL
) DEFAULT CHARSET=utf8mb4;

-- @ddl create_vector
-- @min-version 9.0
CREATE TABLE t_vec (id INT NOT NULL PRIMARY KEY, n INT NULL, v VECTOR(4) NULL);

-- @txn ins_int
INSERT INTO t_int VALUES
  (1, -128, 0, -32768, 0, -8388608, 0, -2147483648, 0, -9223372036854775808, 0),
  (2, 127, 255, 32767, 65535, 8388607, 16777215, 2147483647, 4294967295, 9223372036854775807, 18446744073709551615),
  (3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
  (4, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1),
  (5, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

-- @txn ins_num
INSERT INTO t_num VALUES
  (1, 99999999999999999999999999999999999.999999999999999999999999999999, 9999999999, 0.99999, 99999999.99, 999999999.999999999, 99.99,
   3.4028234e38, 1.7976931348623157e308, b'1', b'1111111111', b'1111111111111111111111111111111111111111111111111111111111111111'),
  (2, -99999999999999999999999999999999999.999999999999999999999999999999, -9999999999, -0.99999, -99999999.99, -999999999.999999999, -99.99,
   -3.4028234e38, -1.7976931348623157e308, b'0', b'0000000000', b'0000000000000000000000000000000000000000000000000000000000000000'),
  (3, -0.000, -0, -0.00000, -0.00, -0.000000000, -0.00,
   -0e0, -0e0, NULL, b'0000000001', b'1000000000000000000000000000000000000000000000000000000000000000'),
  (4, 0.000000000000000000000000000001, 1, 0.00001, 0.01, -0.000000001, -1.5,
   1.17549435e-38, 2.2250738585072014e-308, b'1', b'1000000000', b'0101010101010101010101010101010101010101010101010101010101010101'),
  (5, -12345678901234567890.123456789012345678901234567890, -1234567890, -0.5, 123456.78, 123456789.123456789, 1.05,
   1.401298e-45, 4.9e-324, b'0', b'0101010101', b'0000000000000000000000000000000000000000000000000000000000000001'),
  (6, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

-- @txn ins_time
INSERT INTO t_time (id, d, dt0, dt1, dt2, dt3, dt4, dt5, dt6, ts0, ts1, ts2, ts3, ts4, ts5, ts6, tm0, tm1, tm2, tm3, tm4, tm5, tm6, y) VALUES
  (1, '0000-00-00', '0000-00-00 00:00:00', '0000-00-00 00:00:00.0', '0000-00-00 00:00:00.00', '0000-00-00 00:00:00.000', '0000-00-00 00:00:00.0000', '0000-00-00 00:00:00.00000', '0000-00-00 00:00:00.000000', '0000-00-00 00:00:00', '0000-00-00 00:00:00.0', '0000-00-00 00:00:00.00', '0000-00-00 00:00:00.000', '0000-00-00 00:00:00.0000', '0000-00-00 00:00:00.00000', '0000-00-00 00:00:00.000000', '00:00:00', '00:00:00.0', '00:00:00.00', '00:00:00.000', '00:00:00.0000', '00:00:00.00000', '00:00:00.000000', 0),
  (2, '2024-00-15', '2024-05-00 12:34:56', '2024-05-00 12:34:56.1', '2024-05-00 12:34:56.12', '2024-05-00 12:34:56.123', '2024-05-00 12:34:56.1234', '2024-05-00 12:34:56.12345', '2024-05-00 12:34:56.123456', '1970-01-01 00:00:01', '1970-01-01 00:00:01.0', '1970-01-01 00:00:01.00', '1970-01-01 00:00:01.000', '1970-01-01 00:00:01.0000', '1970-01-01 00:00:01.00000', '1970-01-01 00:00:01.000000', '-00:00:01', '-00:00:00.1', '-00:00:00.01', '-00:00:00.001', '-00:00:00.0001', '-00:00:00.00001', '-00:00:00.000001', 1901),
  (3, '9999-12-31', '9999-12-31 23:59:59', '9999-12-31 23:59:59.9', '9999-12-31 23:59:59.99', '9999-12-31 23:59:59.999', '9999-12-31 23:59:59.9999', '9999-12-31 23:59:59.99999', '9999-12-31 23:59:59.999999', '2038-01-19 03:14:07', '2038-01-19 03:14:07.9', '2038-01-19 03:14:07.99', '2038-01-19 03:14:07.999', '2038-01-19 03:14:07.9999', '2038-01-19 03:14:07.99999', '2038-01-19 03:14:07.999999', '838:59:59', '838:59:59.0', '838:59:59.00', '838:59:59.000', '838:59:59.0000', '838:59:59.00000', '838:59:59.000000', 2155),
  (4, '1000-01-01', '1000-01-01 00:00:00', '1000-01-01 00:00:00.0', '1000-01-01 00:00:00.00', '1000-01-01 00:00:00.000', '1000-01-01 00:00:00.0000', '1000-01-01 00:00:00.00000', '1000-01-01 00:00:00.000000', '1970-01-01 00:00:01', '1970-01-01 00:00:01.0', '1970-01-01 00:00:01.00', '1970-01-01 00:00:01.000', '1970-01-01 00:00:01.0000', '1970-01-01 00:00:01.00000', '1970-01-01 00:00:01.000001', '-838:59:59', '-838:59:59.0', '-838:59:59.00', '-838:59:59.000', '-838:59:59.0000', '-838:59:59.00000', '-838:59:59.000000', 1901),
  (5, '2024-02-29', '2024-02-29 23:59:59', '2024-02-29 23:59:59.1', '2024-02-29 23:59:59.12', '2024-02-29 23:59:59.123', '2024-02-29 23:59:59.1234', '2024-02-29 23:59:59.12345', '2024-02-29 23:59:59.123456', '2024-02-29 12:00:00', '2024-02-29 12:00:00.6', '2024-02-29 12:00:00.65', '2024-02-29 12:00:00.654', '2024-02-29 12:00:00.6543', '2024-02-29 12:00:00.65432', '2024-02-29 12:00:00.654321', '-12:34:56', '-12:34:56.7', '-12:34:56.78', '-12:34:56.789', '-12:34:56.7890', '-12:34:56.78901', '-12:34:56.789012', 2024),
  (6, '2024-00-00', '0000-01-01 00:00:00', '0000-01-01 00:00:00.5', '0000-01-01 00:00:00.50', '0000-01-01 00:00:00.500', '0000-01-01 00:00:00.5000', '0000-01-01 00:00:00.50000', '0000-01-01 00:00:00.500000', '2001-09-09 01:46:40', '2001-09-09 01:46:40.1', '2001-09-09 01:46:40.10', '2001-09-09 01:46:40.100', '2001-09-09 01:46:40.1000', '2001-09-09 01:46:40.10000', '2001-09-09 01:46:40.100000', '-00:59:59', '-00:59:59.9', '-00:59:59.99', '-00:59:59.999', '-00:59:59.9999', '-00:59:59.99999', '-00:59:59.999999', 1999),
  (7, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

-- @txn ins_str
INSERT INTO t_str VALUES
  (1, 'café', 'ÀÿÞ ß', 'abc', REPEAT('ü', 100), REPEAT('😀', 255), '😀🎉', REPEAT('あ', 300),
   X'0001', REPEAT(UNHEX('AB'), 300)),
  (2, '', '', '', '', '', '', '', X'', X''),
  (3, 'ab  ', 'x\ty', 'z ', 'ü日本', '末尾  ', 'q"uote\\', 'line1\nline2\r\u0000', X'FF', X'00'),
  (4, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
  (5, 'é', 'ÿ', 'A', '日本語', '𠮷野家', '𝄞', CONCAT(REPEAT('a', 299), '😀'), X'0102030405060708', X'FFFE');

-- @txn ins_lob
INSERT INTO t_lob VALUES
  (1, 1, '', '', '', '', X'', X'', X'', X''),
  (2, 2, 'tiny', 'text ü', 'medium 😀', 'long', X'01', X'0203', X'00', X'FF'),
  (3, 3, REPEAT('t', 255), REPEAT('x', 300), REPEAT('m', 1000), REPEAT('l', 2000),
   REPEAT(UNHEX('AA'), 255), REPEAT(UNHEX('BB'), 300), REPEAT(UNHEX('CC'), 1000), REPEAT(UNHEX('DD'), 2000)),
  (4, 4, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

-- @txn ins_enum
INSERT INTO t_enum VALUES
  (1, 'a', 'v000', 'x', 'm00', 'b00'),
  (2, 'c', 'v299', 'x,y,z', 'm00,m11', 'b00,b01,b02,b03,b04,b05,b06,b07,b08,b09,b10,b11,b12,b13,b14,b15,b16,b17,b18,b19,b20,b21,b22,b23,b24,b25,b26,b27,b28,b29,b30,b31,b32,b33,b34,b35,b36,b37,b38,b39,b40,b41,b42,b43,b44,b45,b46,b47,b48,b49,b50,b51,b52,b53,b54,b55,b56,b57,b58,b59,b60,b61,b62,b63'),
  (3, '', 'v255', '', 'm08', 'b63'),
  (4, NULL, NULL, NULL, NULL, NULL),
  (5, 'b', 'v256', 'y', 'm07,m08', 'b07,b08,b31,b32');

-- @txn ins_json
INSERT INTO t_json VALUES
  (1, 0, 'null'), (2, 0, 'true'), (3, 0, 'false'), (4, 0, '0'), (5, 0, '-1'),
  (6, 0, '18446744073709551615'), (7, 0, '-9223372036854775808'), (8, 0, '3.14'),
  (9, 0, '1.5e300'), (10, 0, '"str"'),
  (11, 0, '"\\u00e9\\ud83d\\ude00 \\n\\t\\"q\\" \\\\ é😀"'),
  (12, 0, '{}'), (13, 0, '[]'),
  (14, 0, '{"a": 1, "b": [true, false, null, {"c": "d"}], "e": {"f": {"g": [1.5, -2, "h"]}}}'),
  (15, 0, '{"a": 1, "b": "two", "c": [1, 2, 3], "d": {"x": null}}'),
  (16, 0, JSON_OBJECT('d', CAST('2024-01-02' AS DATE), 't', CAST('-12:34:56.5' AS TIME(1)),
     'dt', CAST('2024-01-02 03:04:05.678' AS DATETIME(3)), 'dec', CAST(12.345 AS DECIMAL(10,3)),
     'dneg', CAST(-0.5 AS DECIMAL(5,2)))),
  (17, 0, JSON_ARRAY(CAST(1.50 AS DECIMAL(5,2)), CAST('2038-01-19 03:14:07' AS DATETIME),
     CAST('00:00:00' AS TIME), CAST(4294967295 AS UNSIGNED), -32768, 65535)),
  (18, 0, '{"ключ": "値", "😀": 1, "": "empty key"}'),
  (19, 0, NULL),
  (20, 0, '[1, 2.5, -3, 4294967296, 1e-300, "x", [], {}]'),
  (21, 0, JSON_OBJECT('bin', UNHEX('0102FF'), 'year', CAST(2024 AS UNSIGNED)));

-- @txn ins_big
INSERT INTO t_big VALUES
  (1, 1, REPEAT('m', 65600),
   JSON_OBJECT('big', JSON_ARRAY(REPEAT('a', 65600)), 'n', 1, 's', 'small')),
  (2, 2, 'short', '{"n": 2}');

-- @txn ins_geo
INSERT INTO t_geo VALUES
  (1, ST_GeomFromText('LINESTRING(0 0, 1 1, 2 3)'), ST_GeomFromText('POINT(1.5 -2.25)'),
   ST_GeomFromText('POLYGON((0 0, 0 10, 10 10, 10 0, 0 0))', 4326)),
  (2, ST_GeomFromText('GEOMETRYCOLLECTION(POINT(1 1), LINESTRING(0 0, 1 1))'), POINT(0, 0), NULL),
  (3, ST_GeomFromText('POINT(1 2)', 4326), NULL, NULL),
  (4, NULL, NULL, NULL);

-- @txn ins_vector
-- @min-version 9.0
INSERT INTO t_vec VALUES
  (1, 0, STRING_TO_VECTOR('[1.5, -2, 0, 3.25]')),
  (2, 0, STRING_TO_VECTOR('[0, 0, 0, 0]')),
  (3, 0, NULL);

-- @txn upd_vector
-- @min-version 9.0
UPDATE t_vec SET v = STRING_TO_VECTOR('[1, 2, 3, 4]') WHERE id = 3;
UPDATE t_vec SET n = 1 WHERE id = 1;

-- @txn bulk_insert
INSERT INTO t_bulk (id, a, s)
SELECT d1.n * 1000 + d2.n * 100 + d3.n * 10 + d4.n + 1,
       (d1.n * 1000 + d2.n * 100 + d3.n * 10 + d4.n) * 7,
       CONCAT('r', d1.n, d2.n, d3.n, d4.n)
FROM (SELECT 0 AS n UNION ALL SELECT 1) d1
CROSS JOIN (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
            UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) d2
CROSS JOIN (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
            UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) d3
CROSS JOIN (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
            UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) d4;

-- @txn bulk_update
UPDATE t_bulk SET a = a + 1, s = NULL WHERE id % 3 = 0;
UPDATE t_bulk SET a = -a WHERE id > 1500;

-- @txn bulk_delete
DELETE FROM t_bulk WHERE id > 700;

-- @txn upd_int_partial_columns
UPDATE t_int SET i = 42 WHERE id = 3;
UPDATE t_int SET bu = 18446744073709551614, ti = -127 WHERE id = 2;

-- @txn upd_int_pk_change
UPDATE t_int SET id = 100 WHERE id = 4;

-- @txn upd_null_to_value
UPDATE t_int SET ti = 1, bu = 2 WHERE id = 5;
UPDATE t_time SET d = '2020-01-01', dt6 = '2020-01-01 00:00:00.000001', ts0 = '2020-01-01 00:00:00',
  tm6 = '01:02:03.000004', y = 2000 WHERE id = 7;
UPDATE t_str SET v_s = 'now set', bn = X'01' WHERE id = 4;
UPDATE t_lob SET lt = 'was null', mb = X'0A0B' WHERE id = 4;

-- @txn upd_value_to_null
UPDATE t_num SET d65 = NULL, f = NULL, b64 = NULL WHERE id = 1;
UPDATE t_time SET dt3 = NULL, ts6 = NULL, tm2 = NULL, y = NULL WHERE id = 5;
UPDATE t_str SET c_mb = NULL, vb = NULL WHERE id = 1;
UPDATE t_enum SET e2 = NULL, s8 = NULL WHERE id = 2;

-- @txn upd_lob_blob_only
UPDATE t_lob SET bl = X'DEADBEEF', tx = 'new text' WHERE id = 2;
UPDATE t_lob SET mb = REPEAT(UNHEX('EE'), 500) WHERE id = 3;

-- @txn upd_lob_nonblob_only
UPDATE t_lob SET n = n + 10 WHERE id IN (2, 3);

-- @txn upd_big_nonblob_only
UPDATE t_big SET n = 10 WHERE id = 1;

-- @txn upd_time_values
UPDATE t_time SET dt0 = '2000-01-01 00:00:00', tm3 = '-01:00:00.5', ts3 = '1999-12-31 23:59:59.999' WHERE id = 6;

-- @txn upd_enum_values
UPDATE t_enum SET e2 = 'v298', s2 = 'm01,m09,m10' WHERE id = 1;

-- @txn upd_geo
UPDATE t_geo SET p = ST_GeomFromText('POINT(3 4)') WHERE id = 2;
UPDATE t_geo SET g = NULL WHERE id = 3;

-- @txn upd_json_set_replace
UPDATE t_json SET j = JSON_SET(j, '$.a', 42) WHERE id = 15;
UPDATE t_json SET j = JSON_REPLACE(j, '$.b', 'TWO', '$.c[1]', 20) WHERE id = 15;
UPDATE t_json SET j = JSON_SET(j, '$[0]', 9, '$[5]', 'yy') WHERE id = 20;

-- @txn upd_json_remove
UPDATE t_json SET j = JSON_REMOVE(j, '$.d') WHERE id = 15;
UPDATE t_json SET j = JSON_REMOVE(j, '$.b[1]', '$.e.f') WHERE id = 14;

-- @txn upd_json_set_new_path_and_column
UPDATE t_json SET j = JSON_SET(j, '$.new', JSON_OBJECT('x', 1)), n = n + 1 WHERE id = 14;
UPDATE t_json SET j = JSON_SET(j, '$.s', 'a much longer string than the one before') WHERE id = 18;

-- @txn upd_json_full_and_null
UPDATE t_json SET j = '{"replaced": true}' WHERE id = 2;
UPDATE t_json SET j = NULL WHERE id = 3;
UPDATE t_json SET j = '[]' WHERE id = 19;

-- @txn upd_json_nonjson_only
UPDATE t_json SET n = 5 WHERE id IN (16, 17);

-- @txn upd_big_json_partial
UPDATE t_big SET j = JSON_REPLACE(j, '$.n', 2, '$.s', 'SMALL') WHERE id = 1;
UPDATE t_big SET j = JSON_SET(j, '$.m', 3) WHERE id = 2;

-- @txn tagged_gtid_1
-- @gtid-next AUTOMATIC:mytag
INSERT INTO t_int (id, i) VALUES (200, 1);

-- @txn tagged_gtid_2
-- @gtid-next AUTOMATIC:mytag
UPDATE t_int SET i = 2 WHERE id = 200;

-- @txn after_tagged_gtid
INSERT INTO t_int (id, i) VALUES (201, 3);

-- @txn multi_table
INSERT INTO t_int (id, ti, bu) VALUES (6, 6, 6), (7, 7, 7);
UPDATE t_str SET v_l = 'multi', c_u3 = 'çà' WHERE id = 5;
DELETE FROM t_num WHERE id = 6;
UPDATE t_json SET j = JSON_SET(j, '$.multi', TRUE) WHERE id = 12;

-- @txn deletes
DELETE FROM t_time WHERE id = 1;
DELETE FROM t_str WHERE id IN (2, 3);
DELETE FROM t_lob WHERE id = 1;
DELETE FROM t_enum WHERE id = 4;
DELETE FROM t_json WHERE id IN (1, 4);
DELETE FROM t_geo WHERE id = 4;

-- @txn delete_big
DELETE FROM t_big WHERE id = 1;
