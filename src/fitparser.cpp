#include <cstring>
#include <QtEndian>
#include "fitparser.h"


const quint32 FIT_MAGIC = 0x5449462E; // .FIT


FITParser::FITParser(QList<TrackData> &tracks, QList<RouteData> &routes,
  QList<Waypoint> &waypoints) : Parser(tracks, routes, waypoints)
{
	memset(_defs, 0, sizeof(_defs));
}

FITParser::~FITParser()
{
	for (int i = 0; i < 16; i++)
		if (_defs[i].fields)
			delete[] _defs[i].fields;
}

bool FITParser::readData(char *data, size_t size)
{
	qint64 n;

	n = _device->read(data, size);
	if (n < 0) {
		_errorString = "I/O error";
		return false;
	} else if ((size_t)n < size) {
		_errorString = "Premature end of data";
		return false;
	}

	return true;
}

template<class T> bool FITParser::readValue(T &val)
{
	T data;

	if (!readData((char*)&data, sizeof(T)))
		return false;

	_len -= sizeof(T);

	if (sizeof(T) > 1) {
		if (_endian)
			val = qFromBigEndian(data);
		else
			val = qFromLittleEndian(data);
	} else
		val = data;

	return true;
}

bool FITParser::skipValue(size_t size)
{
	size_t i;
	quint8 val;

	for (i = 0; i < size; i++)
		if (!readValue(val))
			return false;

	return true;
}

bool FITParser::parseDefinitionMessage(quint8 header)
{
	int local_id = header & 0x0f;
	MessageDefinition* def = &_defs[local_id];
	quint8 i;


	if (def->fields)
		delete[] def->fields;

	// reserved/unused
	if (!readValue(i))
		return false;

	// endianness
	if (!readValue(def->endian))
		return false;
	if (def->endian > 1) {
		_errorString = "Bad endian field";
		return false;
	}
	_endian = def->endian;

	// global message number
	if (!readValue(def->global_id))
		return false;

	// number of records
	if (!readValue(def->num_fields))
		return false;
	if (def->num_fields == 0) {
		def->fields = 0;
		return true;
	}

	// definition records
	def->fields = new Field[def->num_fields];
	for (i = 0; i < def->num_fields; i++) {
		if (!readData((char*)&(def->fields[i]), sizeof(def->fields[i])))
			return false;
		_len -= sizeof(def->fields[i]);
	}

	return true;
}

bool FITParser::readField(Field* f, quint32 &val)
{
	quint8 v8 = (quint8)-1;
	quint16 v16 = (quint16)-1;
	bool ret;

	val = (quint32)-1;

	switch (f->type) {
		case 1: // sint8
		case 2: // uint8
			if (f->size == 1) {
				ret = readValue(v8);
				val = v8;
			} else
				ret = skipValue(f->size);
			break;
		case 0x83: // sint16
		case 0x84: // uint16
			if (f->size == 2) {
				ret = readValue(v16);
				val = v16;
			} else
				ret = skipValue(f->size);
			break;
		case 0x85: // sint32
		case 0x86: // uint32
			if (f->size == 4)
				ret = readValue(val);
			else
				ret = skipValue(f->size);
			break;
		default:
			ret = skipValue(f->size);
			break;
	}

	return ret;
}

bool FITParser::parseData(MessageDefinition *def, quint8 offset)
{
	Field *field;
	quint32 timestamp = _timestamp + offset;
	quint32 val;
	Trackpoint trackpoint;
	int i;


	_endian = def->endian;

	for (i = 0; i < def->num_fields; i++) {
		field = &def->fields[i];
		if (!readField(field, val))
			return false;

		if (field->id == 253)
			_timestamp = timestamp = val;
		else if (def->global_id == 20) {
			switch (field->id) {
				case 0:
					if (val != 0x7fffffff)
						trackpoint.rcoordinates().setLat(
						  (val / (double)0x7fffffff) * 180);
					break;
				case 1:
					if (val != 0x7fffffff)
						trackpoint.rcoordinates().setLon(
						  (val / (double)0x7fffffff) * 180);
					break;
				case 2:
					if (val != 0xffff)
						trackpoint.setElevation((val / 5.0) - 500);
					break;
				case 3:
					if (val != 0xff)
						trackpoint.setHeartRate(val);
					break;
				case 6:
					if (val != 0xffff)
						trackpoint.setSpeed(val / 1000.0f);
					break;
				case 13:
					if (val != 0x7f)
						trackpoint.setTemperature(val);
					break;
				default:
					break;

			}
		}
	}

	if (def->global_id == 20 && trackpoint.coordinates().isValid()) {
		trackpoint.setTimestamp(QDateTime::fromTime_t(timestamp + 631065600));
		_tracks.last().append(trackpoint);
	}

	return true;
}

bool FITParser::parseDataMessage(quint8 header)
{
	int local_id = header & 0x1f;
	MessageDefinition* def = &_defs[local_id];
	return parseData(def, 0);
}

bool FITParser::parseCompressedMessage(quint8 header)
{
	int local_id = (header >> 5) & 3;
	MessageDefinition* def = &_defs[local_id];
	return parseData(def, header & 0x1f);
}

bool FITParser::parseRecord()
{
	quint8 header;

	if (!readValue(header))
		return false;

	if (header & 0x80)
		return parseCompressedMessage(header);
	else if (header & 0x40)
		return parseDefinitionMessage(header);
	else
		return parseDataMessage(header);
}

bool FITParser::parseHeader()
{
	FileHeader hdr;
	quint16 crc;
	qint64 len;

	len = _device->read((char*)&hdr, sizeof(hdr));
	if (len < 0) {
		_errorString = "I/O error";
		return false;
	} else if ((size_t)len < sizeof(hdr)
	  || hdr.magic != qToLittleEndian(FIT_MAGIC)) {
		_errorString = "Not a FIT file";
		return false;
	}

	_len = hdr.data_size;

	if (hdr.header_size > 12)
		if (!readData((char *)&crc, sizeof(crc)))
			return false;

	return true;
}

bool FITParser::loadFile(QFile *file)
{
	_device = file;
	_endian = 0;
	_timestamp = 0;

	if (!parseHeader())
		return false;

	_tracks.append(TrackData());

	while (_len)
		if (!parseRecord())
			return false;

	return true;
}