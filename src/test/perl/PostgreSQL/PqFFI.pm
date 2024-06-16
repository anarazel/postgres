
############################################
#
# FFI wrapper for libpq
#
############################################
package PostgreSQL::PqFFI;

use strict;
use warnings FATAL => 'all';

use FFI::Platypus;
use FFI::CheckLib;

use Exporter qw(import);

our @EXPORT = qw (

  CONNECTION_OK
  CONNECTION_BAD
  CONNECTION_STARTED
  CONNECTION_MADE
  CONNECTION_AWAITING_RESPONSE
  CONNECTION_AUTH_OK
  CONNECTION_SETENV
  CONNECTION_SSL_STARTUP
  CONNECTION_NEEDED
  CONNECTION_CHECK_WRITABLE
  CONNECTION_CONSUME
  CONNECTION_GSS_STARTUP
  CONNECTION_CHECK_TARGET
  CONNECTION_CHECK_STANDBY

  PGRES_EMPTY_QUERY
  PGRES_COMMAND_OK
  PGRES_TUPLES_OK
  PGRES_COPY_OUT
  PGRES_COPY_IN
  PGRES_BAD_RESPONSE
  PGRES_NONFATAL_ERROR
  PGRES_FATAL_ERROR
  PGRES_COPY_BOTH
  PGRES_SINGLE_TUPLE
  PGRES_PIPELINE_SYNC
  PGRES_PIPELINE_ABORTED

  PQPING_OK
  PQPING_REJECT
  PQPING_NO_RESPONSE
  PQPING_NO_ATTEMPT

  PQTRANS_IDLE
  PQTRANS_ACTIVE
  PQTRANS_INTRANS
  PQTRANS_INERROR
  PQTRANS_UNKNOWN

  BOOLOID
  BYTEAOID
  CHAROID
  NAMEOID
  INT8OID
  INT2OID
  INT2VECTOROID
  INT4OID
  TEXTOID
  OIDOID
  TIDOID
  XIDOID
  CIDOID
  OIDVECTOROID
  JSONOID
  XMLOID
  XID8OID
  POINTOID
  LSEGOID
  PATHOID
  BOXOID
  POLYGONOID
  LINEOID
  FLOAT4OID
  FLOAT8OID
  UNKNOWNOID
  CIRCLEOID
  MONEYOID
  MACADDROID
  INETOID
  CIDROID
  MACADDR8OID
  ACLITEMOID
  BPCHAROID
  VARCHAROID
  DATEOID
  TIMEOID
  TIMESTAMPOID
  TIMESTAMPTZOID
  INTERVALOID
  TIMETZOID
  BITOID
  VARBITOID
  NUMERICOID
  REFCURSOROID
  UUIDOID
  TSVECTOROID
  GTSVECTOROID
  TSQUERYOID
  JSONBOID
  JSONPATHOID
  TXID_SNAPSHOTOID
  INT4RANGEOID
  NUMRANGEOID
  TSRANGEOID
  TSTZRANGEOID
  DATERANGEOID
  INT8RANGEOID
  INT4MULTIRANGEOID
  NUMMULTIRANGEOID
  TSMULTIRANGEOID
  TSTZMULTIRANGEOID
  DATEMULTIRANGEOID
  INT8MULTIRANGEOID
  RECORDOID
  RECORDARRAYOID
  CSTRINGOID
  VOIDOID
  TRIGGEROID
  EVENT_TRIGGEROID
  BOOLARRAYOID
  BYTEAARRAYOID
  CHARARRAYOID
  NAMEARRAYOID
  INT8ARRAYOID
  INT2ARRAYOID
  INT2VECTORARRAYOID
  INT4ARRAYOID
  TEXTARRAYOID
  OIDARRAYOID
  TIDARRAYOID
  XIDARRAYOID
  CIDARRAYOID
  OIDVECTORARRAYOID
  JSONARRAYOID
  XMLARRAYOID
  XID8ARRAYOID
  POINTARRAYOID
  LSEGARRAYOID
  PATHARRAYOID
  BOXARRAYOID
  POLYGONARRAYOID
  LINEARRAYOID
  FLOAT4ARRAYOID
  FLOAT8ARRAYOID
  CIRCLEARRAYOID
  MONEYARRAYOID
  MACADDRARRAYOID
  INETARRAYOID
  CIDRARRAYOID
  MACADDR8ARRAYOID
  ACLITEMARRAYOID
  BPCHARARRAYOID
  VARCHARARRAYOID
  DATEARRAYOID
  TIMEARRAYOID
  TIMESTAMPARRAYOID
  TIMESTAMPTZARRAYOID
  INTERVALARRAYOID
  TIMETZARRAYOID
  BITARRAYOID
  VARBITARRAYOID
  NUMERICARRAYOID
  REFCURSORARRAYOID
  UUIDARRAYOID
  TSVECTORARRAYOID
  GTSVECTORARRAYOID
  TSQUERYARRAYOID
  JSONBARRAYOID
  JSONPATHARRAYOID
  TXID_SNAPSHOTARRAYOID
  INT4RANGEARRAYOID
  NUMRANGEARRAYOID
  TSRANGEARRAYOID
  TSTZRANGEARRAYOID
  DATERANGEARRAYOID
  INT8RANGEARRAYOID
  INT4MULTIRANGEARRAYOID
  NUMMULTIRANGEARRAYOID
  TSMULTIRANGEARRAYOID
  TSTZMULTIRANGEARRAYOID
  DATEMULTIRANGEARRAYOID
  INT8MULTIRANGEARRAYOID
  CSTRINGARRAYOID

);

# connection status

use constant {
	CONNECTION_OK => 0,
	CONNECTION_BAD => 1,
	# Non-blocking mode only below here

	CONNECTION_STARTED => 2,
	CONNECTION_MADE => 3,
	CONNECTION_AWAITING_RESPONSE => 4,
	CONNECTION_AUTH_OK => 5,
	CONNECTION_SETENV => 6,
	CONNECTION_SSL_STARTUP => 7,
	CONNECTION_NEEDED => 8,
	CONNECTION_CHECK_WRITABLE => 9,
	CONNECTION_CONSUME => 10,
	CONNECTION_GSS_STARTUP => 11,
	CONNECTION_CHECK_TARGET => 12,
	CONNECTION_CHECK_STANDBY => 13,
};

# exec status

use constant {
	PGRES_EMPTY_QUERY => 0,
	PGRES_COMMAND_OK => 1,
	PGRES_TUPLES_OK => 2,
	PGRES_COPY_OUT => 3,
	PGRES_COPY_IN => 4,
	PGRES_BAD_RESPONSE => 5,
	PGRES_NONFATAL_ERROR => 6,
	PGRES_FATAL_ERROR => 7,
	PGRES_COPY_BOTH => 8,
	PGRES_SINGLE_TUPLE => 9,
	PGRES_PIPELINE_SYNC => 10,
	PGRES_PIPELINE_ABORTED => 11,
};

# ping status

use constant {
	PQPING_OK => 0,
	PQPING_REJECT => 1,
	PQPING_NO_RESPONSE => 2,
	PQPING_NO_ATTEMPT => 3,
};

# txn status
use constant {
	PQTRANS_IDLE => 0,
	PQTRANS_ACTIVE => 1,
	PQTRANS_INTRANS => 2,
	PQTRANS_INERROR => 3,
	PQTRANS_UNKNOWN => 4,
};

# type oids
use constant {
	BOOLOID => 16,
	BYTEAOID => 17,
	CHAROID => 18,
	NAMEOID => 19,
	INT8OID => 20,
	INT2OID => 21,
	INT2VECTOROID => 22,
	INT4OID => 23,
	TEXTOID => 25,
	OIDOID => 26,
	TIDOID => 27,
	XIDOID => 28,
	CIDOID => 29,
	OIDVECTOROID => 30,
	JSONOID => 114,
	XMLOID => 142,
	XID8OID => 5069,
	POINTOID => 600,
	LSEGOID => 601,
	PATHOID => 602,
	BOXOID => 603,
	POLYGONOID => 604,
	LINEOID => 628,
	FLOAT4OID => 700,
	FLOAT8OID => 701,
	UNKNOWNOID => 705,
	CIRCLEOID => 718,
	MONEYOID => 790,
	MACADDROID => 829,
	INETOID => 869,
	CIDROID => 650,
	MACADDR8OID => 774,
	ACLITEMOID => 1033,
	BPCHAROID => 1042,
	VARCHAROID => 1043,
	DATEOID => 1082,
	TIMEOID => 1083,
	TIMESTAMPOID => 1114,
	TIMESTAMPTZOID => 1184,
	INTERVALOID => 1186,
	TIMETZOID => 1266,
	BITOID => 1560,
	VARBITOID => 1562,
	NUMERICOID => 1700,
	REFCURSOROID => 1790,
	UUIDOID => 2950,
	TSVECTOROID => 3614,
	GTSVECTOROID => 3642,
	TSQUERYOID => 3615,
	JSONBOID => 3802,
	JSONPATHOID => 4072,
	TXID_SNAPSHOTOID => 2970,
	INT4RANGEOID => 3904,
	NUMRANGEOID => 3906,
	TSRANGEOID => 3908,
	TSTZRANGEOID => 3910,
	DATERANGEOID => 3912,
	INT8RANGEOID => 3926,
	INT4MULTIRANGEOID => 4451,
	NUMMULTIRANGEOID => 4532,
	TSMULTIRANGEOID => 4533,
	TSTZMULTIRANGEOID => 4534,
	DATEMULTIRANGEOID => 4535,
	INT8MULTIRANGEOID => 4536,
	RECORDOID => 2249,
	RECORDARRAYOID => 2287,
	CSTRINGOID => 2275,
	VOIDOID => 2278,
	TRIGGEROID => 2279,
	EVENT_TRIGGEROID => 3838,
	BOOLARRAYOID => 1000,
	BYTEAARRAYOID => 1001,
	CHARARRAYOID => 1002,
	NAMEARRAYOID => 1003,
	INT8ARRAYOID => 1016,
	INT2ARRAYOID => 1005,
	INT2VECTORARRAYOID => 1006,
	INT4ARRAYOID => 1007,
	TEXTARRAYOID => 1009,
	OIDARRAYOID => 1028,
	TIDARRAYOID => 1010,
	XIDARRAYOID => 1011,
	CIDARRAYOID => 1012,
	OIDVECTORARRAYOID => 1013,
	JSONARRAYOID => 199,
	XMLARRAYOID => 143,
	XID8ARRAYOID => 271,
	POINTARRAYOID => 1017,
	LSEGARRAYOID => 1018,
	PATHARRAYOID => 1019,
	BOXARRAYOID => 1020,
	POLYGONARRAYOID => 1027,
	LINEARRAYOID => 629,
	FLOAT4ARRAYOID => 1021,
	FLOAT8ARRAYOID => 1022,
	CIRCLEARRAYOID => 719,
	MONEYARRAYOID => 791,
	MACADDRARRAYOID => 1040,
	INETARRAYOID => 1041,
	CIDRARRAYOID => 651,
	MACADDR8ARRAYOID => 775,
	ACLITEMARRAYOID => 1034,
	BPCHARARRAYOID => 1014,
	VARCHARARRAYOID => 1015,
	DATEARRAYOID => 1182,
	TIMEARRAYOID => 1183,
	TIMESTAMPARRAYOID => 1115,
	TIMESTAMPTZARRAYOID => 1185,
	INTERVALARRAYOID => 1187,
	TIMETZARRAYOID => 1270,
	BITARRAYOID => 1561,
	VARBITARRAYOID => 1563,
	NUMERICARRAYOID => 1231,
	REFCURSORARRAYOID => 2201,
	UUIDARRAYOID => 2951,
	TSVECTORARRAYOID => 3643,
	GTSVECTORARRAYOID => 3644,
	TSQUERYARRAYOID => 3645,
	JSONBARRAYOID => 3807,
	JSONPATHARRAYOID => 4073,
	TXID_SNAPSHOTARRAYOID => 2949,
	INT4RANGEARRAYOID => 3905,
	NUMRANGEARRAYOID => 3907,
	TSRANGEARRAYOID => 3909,
	TSTZRANGEARRAYOID => 3911,
	DATERANGEARRAYOID => 3913,
	INT8RANGEARRAYOID => 3927,
	INT4MULTIRANGEARRAYOID => 6150,
	NUMMULTIRANGEARRAYOID => 6151,
	TSMULTIRANGEARRAYOID => 6152,
	TSTZMULTIRANGEARRAYOID => 6153,
	DATEMULTIRANGEARRAYOID => 6155,
	INT8MULTIRANGEARRAYOID => 6157,
	CSTRINGARRAYOID => 1263,
};



my @procs = qw(

  PQconnectdb
  PQconnectdbParams
  PQsetdbLogin
  PQfinish
  PQreset
  PQdb
  PQuser
  PQpass
  PQhost
  PQhostaddr
  PQport
  PQtty
  PQoptions
  PQstatus
  PQtransactionStatus
  PQparameterStatus
  PQping
  PQpingParams

  PQexec
  PQexecParams
  PQprepare
  PQexecPrepared

  PQdescribePrepared
  PQdescribePortal

  PQclosePrepared
  PQclosePortal
  PQclear

  PQprotocolVersion
  PQserverVersion
  PQerrorMessage
  PQsocket
  PQbackendPID
  PQconnectionNeedsPassword
  PQconnectionUsedPassword
  PQconnectionUsedGSSAPI
  PQclientEncoding
  PQsetClientEncoding

  PQresultStatus
  PQresStatus
  PQresultErrorMessage
  PQresultErrorField
  PQntuples
  PQnfields
  PQbinaryTuples
  PQfname
  PQfnumber
  PQftable
  PQftablecol
  PQfformat
  PQftype
  PQfsize
  PQfmod
  PQcmdStatus
  PQoidValue
  PQcmdTuples
  PQgetvalue
  PQgetlength
  PQgetisnull
  PQnparams
  PQparamtype

);

push(@EXPORT, @procs);

sub setup
{
	my $libdir = shift;

	my $ffi = FFI::Platypus->new(api => 1);

	$ffi->type('opaque' => 'PGconn');
	$ffi->type('opaque' => 'PGresult');
	$ffi->type('uint32' => 'Oid');
	$ffi->type('int' => 'ExecStatusType');

	my $lib = find_lib_or_die(
		lib => 'pq',
		libpath => [$libdir],
		systempath => [],);
	$ffi->lib($lib);

	$ffi->attach('PQconnectdb' => ['string'] => 'PGconn');
	$ffi->attach(
		'PQconnectdbParams' => [ 'string[]', 'string[]', 'int' ] => 'PGconn');
	$ffi->attach(
		'PQsetdbLogin' => [
			'string', 'string', 'string', 'string',
			'string', 'string', 'string',
		] => 'PGconn');
	$ffi->attach('PQfinish' => ['PGconn'] => 'void');
	$ffi->attach('PQreset' => ['PGconn'] => 'void');
	$ffi->attach('PQdb' => ['PGconn'] => 'string');
	$ffi->attach('PQuser' => ['PGconn'] => 'string');
	$ffi->attach('PQpass' => ['PGconn'] => 'string');
	$ffi->attach('PQhost' => ['PGconn'] => 'string');
	$ffi->attach('PQhostaddr' => ['PGconn'] => 'string');
	$ffi->attach('PQport' => ['PGconn'] => 'string');
	$ffi->attach('PQtty' => ['PGconn'] => 'string');
	$ffi->attach('PQoptions' => ['PGconn'] => 'string');
	$ffi->attach('PQstatus' => ['PGconn'] => 'int');
	$ffi->attach('PQtransactionStatus' => ['PGconn'] => 'int');
	$ffi->attach('PQparameterStatus' => [ 'PGconn', 'string' ] => 'string');
	$ffi->attach('PQping' => ['string'] => 'int');
	$ffi->attach(
		'PQpingParams' => [ 'string[]', 'string[]', 'int' ] => 'int');

	$ffi->attach('PQprotocolVersion' => ['PGconn'] => 'int');
	$ffi->attach('PQserverVersion' => ['PGconn'] => 'int');
	$ffi->attach('PQerrorMessage' => ['PGconn'] => 'string');
	$ffi->attach('PQsocket' => ['PGconn'] => 'int');
	$ffi->attach('PQbackendPID' => ['PGconn'] => 'int');
	$ffi->attach('PQconnectionNeedsPassword' => ['PGconn'] => 'int');
	$ffi->attach('PQconnectionUsedPassword' => ['PGconn'] => 'int');
	$ffi->attach('PQconnectionUsedGSSAPI' => ['PGconn'] => 'int');
	$ffi->attach('PQclientEncoding' => ['PGconn'] => 'int');
	$ffi->attach('PQsetClientEncoding' => [ 'PGconn', 'string' ] => 'int');

	$ffi->attach('PQexec' => [ 'PGconn', 'string' ] => 'PGresult');
	$ffi->attach(
		'PQexecParams' => [
			'PGconn', 'string', 'int', 'int[]',
			'string[]', 'int[]', 'int[]', 'int'
		] => 'PGresult');
	$ffi->attach(
		'PQprepare' => [ 'PGconn', 'string', 'string', 'int', 'int[]' ] =>
		  'PGresult');
	$ffi->attach(
		'PQexecPrepared' => [ 'PGconn', 'string', 'int',
			'string[]', 'int[]', 'int[]', 'int' ] => 'PGresult');

	$ffi->attach('PQresultStatus' => ['PGresult'] => 'ExecStatusType');
	$ffi->attach('PQresStatus' => ['ExecStatusType'] => 'string');
	$ffi->attach('PQresultErrorMessage' => ['PGresult'] => 'string');
	$ffi->attach('PQresultErrorField' => [ 'PGresult', 'int' ] => 'string');
	$ffi->attach('PQntuples' => ['PGresult'] => 'int');
	$ffi->attach('PQnfields' => ['PGresult'] => 'int');
	$ffi->attach('PQbinaryTuples' => ['PGresult'] => 'int');
	$ffi->attach('PQfname' => [ 'PGresult', 'int' ] => 'string');
	$ffi->attach('PQfnumber' => [ 'PGresult', 'string' ] => 'int');
	$ffi->attach('PQftable' => [ 'PGresult', 'int' ] => 'Oid');
	$ffi->attach('PQftablecol' => [ 'PGresult', 'int' ] => 'int');
	$ffi->attach('PQfformat' => [ 'PGresult', 'int' ] => 'int');
	$ffi->attach('PQftype' => [ 'PGresult', 'int' ] => 'Oid');
	$ffi->attach('PQfsize' => [ 'PGresult', 'int' ] => 'int');
	$ffi->attach('PQfmod' => [ 'PGresult', 'int' ] => 'int');
	$ffi->attach('PQcmdStatus' => ['PGresult'] => 'string');
	$ffi->attach('PQoidValue' => ['PGresult'] => 'Oid');
	$ffi->attach('PQcmdTuples' => ['PGresult'] => 'string');
	$ffi->attach('PQgetvalue' => [ 'PGresult', 'int', 'int' ] => 'string');
	$ffi->attach('PQgetlength' => [ 'PGresult', 'int', 'int' ] => 'int');
	$ffi->attach('PQgetisnull' => [ 'PGresult', 'int', 'int' ] => 'int');
	$ffi->attach('PQnparams' => ['PGresult'] => 'int');
	$ffi->attach('PQparamtype' => [ 'PGresult', 'int' ] => 'Oid');


	$ffi->attach(
		'PQdescribePrepared' => [ 'PGconn', 'string' ] => 'PGresult');
	$ffi->attach('PQdescribePortal' => [ 'PGconn', 'string' ] => 'PGresult');

	$ffi->attach('PQclosePrepared' => [ 'PGconn', 'string' ] => 'PGresult');
	$ffi->attach('PQclosePortal' => [ 'PGconn', 'string' ] => 'PGresult');
	$ffi->attach('PQclear' => ['PGresult'] => 'void');
}


1;
