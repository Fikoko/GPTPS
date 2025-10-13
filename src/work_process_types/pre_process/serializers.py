# serializers.py
import json
import yaml
import toml
import dicttoxml
import xmltodict
import msgpack
import avro.schema
import avro.io
import io
from google.protobuf.message import Message as ProtobufMessage

# ------------------ Generic Serializer ------------------
class Serializer:
    @staticmethod
    def serialize(data, fmt: str):
        fmt = fmt.lower()
        if fmt == "json":
            return json.dumps(data).encode("utf-8")
        elif fmt == "yaml":
            return yaml.dump(data).encode("utf-8")
        elif fmt == "toml":
            return toml.dumps(data).encode("utf-8")
        elif fmt == "xml":
            return dicttoxml.dicttoxml(data)
        elif fmt == "msgpack":
            return msgpack.packb(data)
        elif fmt == "avro":
            if "avro_schema" not in data:
                raise ValueError("Avro serialization requires 'avro_schema' in data")
            schema = avro.schema.parse(data.pop("avro_schema"))
            writer = avro.io.DatumWriter(schema)
            bytes_io = io.BytesIO()
            encoder = avro.io.BinaryEncoder(bytes_io)
            writer.write(data, encoder)
            return bytes_io.getvalue()
        elif fmt == "protobuf":
            if not isinstance(data, ProtobufMessage):
                raise TypeError("Protobuf serialization requires a protobuf Message instance")
            return data.SerializeToString()
        else:
            raise ValueError(f"Unsupported serialization format: {fmt}")

    @staticmethod
    def deserialize(data_bytes, fmt: str, avro_schema=None, protobuf_class=None):
        fmt = fmt.lower()
        if fmt == "json":
            return json.loads(data_bytes.decode("utf-8"))
        elif fmt == "yaml":
            return yaml.safe_load(data_bytes.decode("utf-8"))
        elif fmt == "toml":
            return toml.loads(data_bytes.decode("utf-8"))
        elif fmt == "xml":
            return xmltodict.parse(data_bytes)
        elif fmt == "msgpack":
            return msgpack.unpackb(data_bytes)
        elif fmt == "avro":
            if avro_schema is None:
                raise ValueError("Avro deserialization requires 'avro_schema'")
            schema = avro.schema.parse(avro_schema)
            reader = avro.io.DatumReader(schema)
            bytes_io = io.BytesIO(data_bytes)
            decoder = avro.io.BinaryDecoder(bytes_io)
            return reader.read(decoder)
        elif fmt == "protobuf":
            if protobuf_class is None:
                raise ValueError("Protobuf deserialization requires 'protobuf_class'")
            instance = protobuf_class()
            instance.ParseFromString(data_bytes)
            return instance
        else:
            raise ValueError(f"Unsupported deserialization format: {fmt}")
