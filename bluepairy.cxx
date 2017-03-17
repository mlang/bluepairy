#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <boost/program_options.hpp>

#include <dbus/dbus.h>

namespace bluez {
  struct error: std::runtime_error {
    error(char const *message) : std::runtime_error{message} {}
  };
  struct connection_attempt_failed: error {
    connection_attempt_failed(char const *message) : error{message} {}
  };
}

namespace {

char const * const HIDP = "00000011-0000-1000-8000-00805f9b34fb";
char const * const SPP  = "00001101-0000-1000-8000-00805f9b34fb";

DBusConnection *systemBus;

class Object {
  std::string const dbus_path;

protected:
  Object(std::string const &path) : dbus_path{path} {}

public:
  std::string const &path() const { return dbus_path; }
};

class Device;

class Adapter : public Object {
  std::string property_address;
  bool property_powered;
  bool property_discovering;

public:
  Adapter(std::string const &path) : Object{path} {};

  void on_properties_changed(DBusMessageIter *);

  std::string address() const { return property_address; }
  bool powered() const { return property_powered; }
  void powered(bool);
  bool discovering() const { return property_discovering; }

  void start_discovery() const;
  void remove_device(Device const &) const;
};

class Device : public Object {
  std::shared_ptr<Adapter> property_adapter;
  std::string property_address;
  std::string property_name;
  bool property_paired;
  bool property_connected;
  std::set<std::string> property_uuids;

public:
  Device(std::string const &path) : Object{path} {};
  void on_properties_changed(DBusMessageIter *);

  std::string address() const { return property_address; }
  std::shared_ptr<Adapter const> adapter() const { return property_adapter; }
  std::string name() const { return property_name; }
  bool paired() const { return property_paired; }
  bool connected() const { return property_connected; }
  std::set<std::string> const &uuids() const { return property_uuids; }

  void pair() const;

  void forget() const { adapter()->remove_device(*this); }
};

std::vector<std::shared_ptr<Adapter>> adapters;

std::shared_ptr<Adapter> get_adapter(char const *path) {
  auto pos = std::find_if(adapters.begin(), adapters.end(),
		   [path](auto &adapter) {
		     return adapter->path() == path;
		   });
  if (pos != adapters.end()) return *pos;

  adapters.push_back(std::make_shared<Adapter>(path));
  return adapters.back();
}

  std::vector<std::shared_ptr<Device>> devices;

Device &get_device(char const *path) {
  auto pos = std::find_if(devices.begin(), devices.end(),
			  [path](auto const &device) {
			    return device->path() == path;
    		          });
  if (pos != devices.end()) return **pos;

  devices.push_back(std::make_shared<Device>(path));
  return *devices.back();
}

enum State {
  no_adapter = 0,
  no_power,
  wait_for_power_on,
  search_friendly_name,
  device_misses_profile,
  device_not_paired,
  device_not_connected,
  start_discovery,
  starting_discovery,
  search_friendly_name_while_discovering,
  ready
} state = no_adapter;

void init_bus();
void get_bluez_objects();
void register_agent();
void read_messages();

}

int main(int argc, char *argv[]) {
  std::string friendly_name;
  std::vector<std::string> uuids;

  using namespace boost::program_options;

  options_description desc("Allowed options");
  desc.add_options()
  ("help,?", "print usage message")
  ("friendly-name,n", value(&friendly_name)->required(),
   "Device name (regex)")
  ("profile-uuid,u", value(&uuids), "UUID (regex)")
  ("hidp", "Require HID profile to be present")
  ;
  positional_options_description positional_desc;
  positional_desc.add("friendly-name", 1);
  variables_map vm;
  try {
    store(command_line_parser(argc, argv)
	  .options(desc)
	  .positional(positional_desc)
	  .run(), vm);
    notify(vm);
  } catch (unknown_option &e) {
    std::cerr << e.what() << std::endl << std::endl << desc << std::endl;
    return EXIT_FAILURE;
  } catch (required_option &e) {
    std::cerr << e.what() << std::endl << std::endl << desc << std::endl;
    return EXIT_FAILURE;
  } catch (invalid_command_line_syntax &e) {
    std::cerr << e.what () << std::endl << desc << std::endl;
    return EXIT_FAILURE;
  }

  if (vm.count("help")) {
    std::cout << desc << std::endl;
    return EXIT_SUCCESS;
  }

  if (friendly_name.empty()) {
    std::cerr << "Empty friendly name is not allowed." << std::endl;
    return EXIT_FAILURE;
  }

  for (auto &uuid: uuids) {
    if (uuid.empty()) {
      std::cerr << "Empty UUIDs are not allowed." << std::endl;
      return EXIT_FAILURE;
    }
  }

  if (vm.count("hidp")) {
    uuids.push_back(HIDP);
  }

  if (!uuids.empty()) {
    std::sort(std::begin(uuids), std::end(uuids));
    std::cout << "Bluetooth Profile UUIDs required to be offered by "
	      << "the device:" << std::endl;
    copy(uuids.begin(), uuids.end(),
	 std::ostream_iterator<std::string>(std::cout, "\n"));
  }

  init_bus();
  read_messages();
  get_bluez_objects();
  register_agent();

  {
    std::regex pattern{friendly_name};
    auto search_name = [&pattern](auto device) {
      return regex_search(device->name(), pattern,
			  std::regex_constants::match_not_null);
    };
    auto has_profiles = [&uuids](auto device) {
      std::vector<std::string> intersection;
      std::set_intersection(device->uuids().begin(), device->uuids().end(),
			    uuids.begin(), uuids.end(),
			    std::back_inserter(intersection));
      return intersection == uuids;
    };
    auto usable = [search_name, has_profiles](auto device) {
      return device->paired() && device->adapter()->powered() &&
             search_name(device) && has_profiles(device);
    };
    auto dev = find_if(begin(devices), end(devices), usable);
    if (dev != end(devices)) {
      std::cout << "Apparently usable pairing with "
		<< (*dev)->name() << " (" << (*dev)->address() << ") via "
		<< (*dev)->adapter()->address() << " found, good luck!"
		<< std::endl;

      return EXIT_SUCCESS;
    }
    auto unpaired = [search_name, has_profiles](auto device) {
      return device->adapter()->powered() && !device->paired() &&
             search_name(device) && has_profiles(device);
    };
    dev = find_if(begin(devices), end(devices), unpaired);
    if (dev != devices.end()) {
      std::cout << "Trying to pair with " << (*dev)->name() << "." << std::endl;
      (*dev)->pair();
    }
  }
  while (state != ready) {
    read_messages();

    switch (state) {
    case no_adapter: {
      if (!adapters.empty()) {
	bool power = false;
	for (auto adapter: adapters) {
	  if (adapter->powered()) power = true;
	}

	if (power) state = search_friendly_name;
	else state = no_power;
      }				      

      if (state == no_adapter) {
	std::cerr << "No adapter present." << std::endl;
	return EXIT_FAILURE;
      }

      break;
    }

    case no_power: {
      for (auto adapter: adapters) {
	std::cerr << "Powering up controller " << adapter->address() << std::endl;
	if (!adapter->powered()) {
	  adapter->powered(true);
	  state = wait_for_power_on;
	}
      }
			      
      if (state == no_power)
	return EXIT_FAILURE;

      break;
    }

    case wait_for_power_on:
      for (int retries = 0; retries < 10; ++retries) {
	read_messages();
	bool power = false;
	for (auto adapter: adapters) {
	  if (adapter->powered()) power = true;
	}
	if (power) {
	  state = search_friendly_name;
	  break;
	}
      }

      if (state == no_power) {
	std::cerr << "Failed to power up controller." << std::endl;
	return EXIT_FAILURE;
      }

      break;

    case search_friendly_name: {
      std::regex const pattern(friendly_name);
      auto flags = std::regex_constants::match_not_null;
      for (auto &device: devices) {
	if (regex_search(device->name(), pattern, flags)) {
	  std::cout << "Device " << device->name()
		    << " (" << device->address() << ") "
		    << "matches." << std::endl;
	  if (device->paired()) {
	    std::cout << "Device is paired." << std::endl;
	    for (auto &uuid: uuids) {
	      auto profiles = device->uuids();
	      if (profiles.find(uuid) == profiles.end()) {
		std::cerr << "Device " << device->name() << " does not offer " << uuid << std::endl;
	      }
	    }
	    state = ready;
	  }
	}
      }

      if (state == search_friendly_name) {
	std::cout << "Friendly name matching "
		  << friendly_name
		  << " not found."
		  << std::endl;

	state = start_discovery;
      }

      break;
    }

    case start_discovery: {
      for (auto adapter: adapters) {
	adapter->start_discovery();
	state = starting_discovery;
      }

      if (state == start_discovery)
	return EXIT_FAILURE;

      break;
    }

    case starting_discovery: {
      for (auto adapter: adapters) {
	if (adapter->discovering()) {
	  std::cout << "Adapter is discovering." << std::endl;
	  state = search_friendly_name_while_discovering;
	}
      }
      break;
    }

    case search_friendly_name_while_discovering: {
      static int tries = 0;
      std::cout << "Retry " << tries << std::endl;
      tries++;

      break;
    }

    default:
      fprintf(stderr, "Unhandled state %d\n", state);
      return EXIT_FAILURE;
    }
  }

  dbus_connection_unref(systemBus);

  return EXIT_SUCCESS;
}

namespace {

char const * const BLUEZ_SERVICE     = "org.bluez";

char const * const ADAPTER_INTERFACE = "org.bluez.Adapter1";
char const * const DEVICE_INTERFACE  = "org.bluez.Device1";
char const * const ADAPTER           = "Adapter";
char const * const ADDRESS           = "Address";
char const * const CONNECTED         = "Connected";
char const * const PAIRED            = "Paired";
char const * const POWERED           = "Powered";

void Adapter::on_properties_changed(DBusMessageIter *properties) {
  while (DBUS_TYPE_DICT_ENTRY == dbus_message_iter_get_arg_type(properties)) {
    DBusMessageIter property;
    dbus_message_iter_recurse(properties, &property);
    if (DBUS_TYPE_STRING ==
	dbus_message_iter_get_arg_type(&property)) {
      char const *propertyName;
      dbus_message_iter_get_basic(&property,
				  &propertyName);
      dbus_message_iter_next(&property);
      if (DBUS_TYPE_VARIANT == dbus_message_iter_get_arg_type(&property)) {
	DBusMessageIter value;
	dbus_message_iter_recurse(&property, &value);
	if (strcmp(POWERED, propertyName) == 0) {
	  if (DBUS_TYPE_BOOLEAN == dbus_message_iter_get_arg_type(&value)) {
	    dbus_bool_t boolValue;
	    dbus_message_iter_get_basic(&value, &boolValue);
	    property_powered = boolValue == TRUE;
	  }
	} else if (strcmp(ADDRESS, propertyName) == 0) {
	  if (DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&value)) {
	    char const *stringValue;
	    dbus_message_iter_get_basic(&value, &stringValue);
	    property_address = stringValue;
	  }
	}
	assert(!dbus_message_iter_has_next(&property));
      }
    }
    dbus_message_iter_next(properties);
  }
}

void Adapter::powered(bool value) {
  DBusMessage *msg =
    dbus_message_new_method_call(BLUEZ_SERVICE, path().c_str(),
				 "org.freedesktop.DBus.Properties", "Set");

  if (msg) {
    DBusPendingCall *pending;
    DBusMessageIter args;

    dbus_message_iter_init_append(msg, &args);

    dbus_message_iter_append_basic(&args,
				   DBUS_TYPE_STRING, &ADAPTER_INTERFACE);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &POWERED);
    {
      DBusMessageIter variant;
      dbus_bool_t boolValue = value? TRUE : FALSE;
      dbus_message_iter_open_container(&args,
				       DBUS_TYPE_VARIANT, "b", &variant);
      dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &boolValue);
      dbus_message_iter_close_container(&args, &variant);
    }

    if (dbus_connection_send_with_reply(systemBus, msg, &pending, -1)) {
      if (pending) {
	dbus_connection_flush(systemBus);
	dbus_message_unref(msg);
	dbus_pending_call_block(pending);
	msg = dbus_pending_call_steal_reply(pending);
	if (msg) {
	  DBusError error;
	  dbus_error_init(&error);
	  dbus_set_error_from_message(&error, msg);
	  if (dbus_error_is_set(&error)) {
	    std::runtime_error e(error.message);
	    dbus_error_free(&error);
	    throw e;
	  }
	  dbus_message_unref(msg);
	} else {
	  fprintf(stderr, "reply message is NULL.\n");
	}
	dbus_pending_call_unref(pending);
      } else {
	fprintf(stderr, "pending == NULL\n");
      }
    } else {
      fprintf(stderr, "Failed to send message.\n");
    }
  } else {
    fprintf(stderr, "Failed to allocate method call message.\n");
  }
}

void Adapter::start_discovery() const {
  DBusMessage *msg =
    dbus_message_new_method_call(BLUEZ_SERVICE, path().c_str(),
				 ADAPTER_INTERFACE, "StartDiscovery");

  if (msg) {
    DBusPendingCall *pending;

    if (dbus_connection_send_with_reply(systemBus, msg, &pending, -1)) {
      if (pending) {
	dbus_connection_flush(systemBus);
	dbus_message_unref(msg);
	dbus_pending_call_block(pending);
	msg = dbus_pending_call_steal_reply(pending);
	if (msg) {
	  DBusError error;
	  dbus_error_init(&error);
	  dbus_set_error_from_message(&error, msg);
	  dbus_message_unref(msg);
	  if (dbus_error_is_set(&error)) {
	    std::runtime_error e(error.message);
	    dbus_error_free(&error);
	    throw e;
	  }
	} else {
	  fprintf(stderr, "reply message is NULL.\n");
	}
	dbus_pending_call_unref(pending);
      } else {
	fprintf(stderr, "pending == NULL\n");
      }
    } else {
      fprintf(stderr, "Failed to send message.\n");
    }
  } else {
    fprintf(stderr, "Failed to allocate method call message.\n");
  }
}

void Adapter::remove_device(Device const &device) const {
  DBusMessage *msg =
    dbus_message_new_method_call(BLUEZ_SERVICE, path().c_str(),
				 ADAPTER_INTERFACE, "RemoveDevice");

  if (msg) {
    {
      DBusMessageIter args;
      dbus_message_iter_init_append(msg, &args);
      dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH,
				     device.path().c_str());
    }
    DBusPendingCall *pending;

    if (dbus_connection_send_with_reply(systemBus, msg, &pending, -1)) {
      if (pending) {
	dbus_connection_flush(systemBus);
	dbus_message_unref(msg);
	dbus_pending_call_block(pending);
	msg = dbus_pending_call_steal_reply(pending);
	if (msg) {
	  DBusError error;
	  dbus_error_init(&error);
	  dbus_set_error_from_message(&error, msg);
	  dbus_message_unref(msg);
	  if (dbus_error_is_set(&error)) {
	    std::runtime_error e(error.message);
	    dbus_error_free(&error);
	    throw e;
	  }
	} else {
	  fprintf(stderr, "reply message is NULL.\n");
	}
	dbus_pending_call_unref(pending);
      } else {
	fprintf(stderr, "pending == NULL\n");
      }
    } else {
      fprintf(stderr, "Failed to send message.\n");
    }
  } else {
    fprintf(stderr, "Failed to allocate method call message.\n");
  }
}

void Device::on_properties_changed(DBusMessageIter *properties) {
  while (DBUS_TYPE_DICT_ENTRY == dbus_message_iter_get_arg_type(properties)) {
    DBusMessageIter property;
    dbus_message_iter_recurse(properties, &property);
    if (DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&property)) {
      char const *propertyName;
      dbus_message_iter_get_basic(&property,
				  &propertyName);
      dbus_message_iter_next(&property);
      if (DBUS_TYPE_VARIANT ==
	  dbus_message_iter_get_arg_type(&property)) {
	DBusMessageIter value;
	dbus_message_iter_recurse(&property, &value);
	if (strcmp("Name", propertyName) == 0) {
	  if (DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&value)) {
	    char const *stringValue;
	    dbus_message_iter_get_basic(&value, &stringValue);
	    property_name = stringValue;
	  }
	} else if (strcmp(ADDRESS, propertyName) == 0) {
	  if (DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&value)) {
	    char const *stringValue;
	    dbus_message_iter_get_basic(&value, &stringValue);
	    property_address = stringValue;
	  }
	} else if (strcmp(PAIRED, propertyName) == 0) {
	  if (DBUS_TYPE_BOOLEAN ==
	      dbus_message_iter_get_arg_type(&value)) {
	    dbus_bool_t boolValue;
	    dbus_message_iter_get_basic(&value, &boolValue);
	    property_paired = boolValue == TRUE;
	  }
	} else if (strcmp(CONNECTED, propertyName) == 0) {
	  if (DBUS_TYPE_BOOLEAN ==
	      dbus_message_iter_get_arg_type(&value)) {
	    dbus_bool_t boolValue;
	    dbus_message_iter_get_basic(&value, &boolValue);
	    property_connected = boolValue == TRUE;
	  }
	} else if (strcmp("UUIDs", propertyName) == 0) {
	  if (DBUS_TYPE_ARRAY == dbus_message_iter_get_arg_type(&value)) {
	    DBusMessageIter uuids;

	    dbus_message_iter_recurse(&value, &uuids);

	    property_uuids.clear();
	    while (DBUS_TYPE_STRING ==
		   dbus_message_iter_get_arg_type(&uuids)) {
	      char const *uuid;
	      dbus_message_iter_get_basic(&uuids, &uuid);

	      property_uuids.insert(uuid);
	      dbus_message_iter_next(&uuids);
	    }
	  }
	} else if (strcmp(ADAPTER, propertyName) == 0) {
	  if (DBUS_TYPE_OBJECT_PATH ==
	      dbus_message_iter_get_arg_type(&value)) {
	    char const *objectPath;
	    dbus_message_iter_get_basic(&value, &objectPath);
	    if (objectPath) {
	      property_adapter = get_adapter(objectPath);
	    } else {
	      property_adapter.reset();
	    }
	  }
	}
	assert(!dbus_message_iter_has_next(&property));
      }
    }
    dbus_message_iter_next(properties);
  }
}

void Device::pair() const {
  DBusMessage *msg =
    dbus_message_new_method_call(BLUEZ_SERVICE, path().c_str(),
				 DEVICE_INTERFACE, "Pair");

  if (msg) {
    dbus_uint32_t serial = 1;
    if (dbus_connection_send(systemBus, msg, &serial)) {
      dbus_connection_flush(systemBus);
      dbus_message_unref(msg);
      while (true) {
	read_messages();
      }
    } else {
      fprintf(stderr, "Failed to send message.\n");
    }
  } else {
    fprintf(stderr, "Failed to allocate method call message.\n");
  }
}

void init_bus() {
  DBusError err;

  dbus_error_init(&err);
  systemBus = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
  if (dbus_error_is_set(&err)) {
    std::runtime_error e(err.message);
    dbus_error_free(&err);
    throw e;
  }
  if (systemBus == NULL) throw std::bad_alloc();
  dbus_bus_add_match(systemBus, "type='signal',sender='org.bluez'", &err);
  if (dbus_error_is_set(&err)) {
    std::runtime_error e(err.message);
    dbus_error_free(&err);
    dbus_connection_unref(systemBus);
    throw e;
  }
}

DBusMessage *get_managed_objects(char const *path, char const *interface) {
  DBusMessage *msg =
    dbus_message_new_method_call(interface, path,
				 "org.freedesktop.DBus.ObjectManager",
				 "GetManagedObjects");

  if (msg) {
    DBusPendingCall *pending;
    if (!dbus_connection_send_with_reply(systemBus, msg, &pending, -1)) {
      fprintf(stderr, "Failed to send!\n");
      return NULL;
    }
    if (pending == NULL) {
      fprintf(stderr, "PendingCall == NULL\n");
      return NULL;
    }
    dbus_connection_flush(systemBus);
    dbus_message_unref(msg);
    dbus_pending_call_block(pending);
    msg = dbus_pending_call_steal_reply(pending);
    if (msg == NULL) {
      fprintf(stderr, "Reply == NULL\n");
      return NULL;
    }
    dbus_pending_call_unref(pending);
  } else {
    fprintf(stderr,
	    "Failed to allocate GetManagedObjects method call message.\n");
  }

  return msg;
}

void get_bluez_objects() {
  DBusMessage *msg = get_managed_objects("/", "org.bluez");
  DBusMessageIter args;

  if (msg == NULL) {
    throw std::runtime_error("GetManagedObjects didn't reply");
  }

  if (!dbus_message_iter_init(msg, &args)) {
    throw std::runtime_error("GetManagedObjects reply was empty");
  }

  if (DBUS_TYPE_ARRAY == dbus_message_iter_get_arg_type(&args)) {
    DBusMessageIter objects;

    dbus_message_iter_recurse(&args, &objects);
    while (DBUS_TYPE_DICT_ENTRY == dbus_message_iter_get_arg_type(&objects)) {
      DBusMessageIter object;

      dbus_message_iter_recurse(&objects, &object);
      if (DBUS_TYPE_OBJECT_PATH == dbus_message_iter_get_arg_type(&object)) {
	char const *path;

	dbus_message_iter_get_basic(&object, &path);
	dbus_message_iter_next(&object);
	if (DBUS_TYPE_ARRAY == dbus_message_iter_get_arg_type(&object)) {
	  DBusMessageIter interfaces;

	  dbus_message_iter_recurse(&object, &interfaces);
	  while (DBUS_TYPE_DICT_ENTRY ==
		 dbus_message_iter_get_arg_type(&interfaces)) {
	    DBusMessageIter interface;

	    dbus_message_iter_recurse(&interfaces, &interface);
	    if (DBUS_TYPE_STRING ==
		dbus_message_iter_get_arg_type(&interface)) {
	      char const *interfaceName;

	      dbus_message_iter_get_basic(&interface, &interfaceName);
	      if (strcmp(ADAPTER_INTERFACE, interfaceName) == 0) {
		auto adapter = get_adapter(path);

		dbus_message_iter_next(&interface);
		if (DBUS_TYPE_ARRAY ==
		    dbus_message_iter_get_arg_type(&interface)) {
		  DBusMessageIter properties;

		  dbus_message_iter_recurse(&interface, &properties);
		  adapter->on_properties_changed(&properties);

		  assert(!dbus_message_iter_has_next(&interface));
		}
	      } else if (strcmp(DEVICE_INTERFACE, interfaceName) == 0) {
		auto &device = get_device(path);

		dbus_message_iter_next(&interface);
		if (DBUS_TYPE_ARRAY ==
		    dbus_message_iter_get_arg_type(&interface)) {
		  DBusMessageIter properties;
		  dbus_message_iter_recurse(&interface, &properties);
		  device.on_properties_changed(&properties);
		  assert(!dbus_message_iter_has_next(&interface));
		}
	      }
	    }
	    dbus_message_iter_next(&interfaces);
	  }
	  assert(!dbus_message_iter_has_next(&object));
	}
      }
      dbus_message_iter_next(&objects);
    }
    assert(!dbus_message_iter_has_next(&args));
  }

  dbus_message_unref(msg);
}

void register_agent() {
  DBusMessage *msg =
    dbus_message_new_method_call(BLUEZ_SERVICE, "/org/bluez",
				 "org.bluez.AgentManager1", "RegisterAgent");

  if (msg) {
    DBusMessageIter args;

    dbus_message_iter_init_append(msg, &args);

    char const * const AGENT_PATH = "/bluepair";
    char const * const CAPABILITIES = "DisplayYesNo";
    dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &AGENT_PATH);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &CAPABILITIES);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(systemBus, msg, -1, &error);
    if (dbus_error_is_set(&error)) {
      std::runtime_error e(error.message);
      dbus_error_free(&error);
      throw e;
    }
    dbus_message_unref(msg);
    dbus_message_unref(reply);
    dbus_error_free(&error);
  } else {
    throw std::runtime_error("Failed to allocate method call message");
  }
}

void read_messages() {
  DBusMessage *incoming;

  dbus_connection_read_write(systemBus, 100);

  while ((incoming = dbus_connection_pop_message(systemBus))) {
    char const *path = dbus_message_get_path(incoming);

    switch (dbus_message_get_type(incoming)) {
    case DBUS_MESSAGE_TYPE_ERROR: {
      DBusError error;
      dbus_error_init(&error);
      if (dbus_set_error_from_message(&error, incoming)) {
	if (strcmp("org.bluez.Error.ConnectionAttemptFailed", error.name) == 0) {
	  bluez::connection_attempt_failed e(error.message);
	  dbus_error_free(&error);
	  dbus_message_unref(incoming);
	  throw e;
	} else {
	  std::runtime_error e(std::string{error.name} + ": " + error.message);
	  dbus_error_free(&error);
	  dbus_message_unref(incoming);
	  throw e;
	}
      }
      break;
    }
    case DBUS_MESSAGE_TYPE_METHOD_CALL:
      std::cerr << "Method call" << std::endl;
      break;
    case DBUS_MESSAGE_TYPE_METHOD_RETURN:
      std::cout << "Method return" << std::endl;
      break;
    case DBUS_MESSAGE_TYPE_SIGNAL:
      if (dbus_message_has_interface(incoming,
				     "org.freedesktop.DBus.Properties") &&
	  dbus_message_has_member(incoming, "PropertiesChanged")) {
	DBusMessageIter args;

	dbus_message_iter_init(incoming, &args);

	if (DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&args)) {
	  char const *interface;

	  dbus_message_iter_get_basic(&args, &interface);
	  dbus_message_iter_next(&args);
	  if (DBUS_TYPE_ARRAY == dbus_message_iter_get_arg_type(&args)) {
	    DBusMessageIter properties;

	    dbus_message_iter_recurse(&args, &properties);

	    if (strcmp(ADAPTER_INTERFACE, interface) == 0) {
	      auto adapter = get_adapter(path);

	      adapter->on_properties_changed(&properties);
	    } else if (strcmp(DEVICE_INTERFACE, interface) == 0) {
	      auto &device = get_device(path);

	      device.on_properties_changed(&properties);
	    }
	  }
	}
      } else if (dbus_message_has_interface(incoming, "org.freedesktop.DBus.ObjectManager")) {
	if (dbus_message_has_member(incoming, "InterfacesAdded")) {
	} else if (dbus_message_has_member(incoming, "InterfacesRemoved")) {
	}
      }
      fprintf(stderr, "Signal %s.%s\n", dbus_message_get_interface(incoming), dbus_message_get_member(incoming));
      break;
    }
    dbus_message_unref(incoming);
  }
}

}
