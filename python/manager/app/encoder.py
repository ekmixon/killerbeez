import json

class JSONEncoder(json.JSONEncoder):
    def default(self, o):
        try:
            return o.timestamp()
        except AttributeError:
            pass

        return super(JSONEncoder, self).default(o)
