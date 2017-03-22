#include <chrono>
#include <iostream>

#include <boost/program_options.hpp>

#include "bluepairy.hxx"

int main(int argc, char *argv[])
{
  std::string FriendlyName;
  std::vector<std::string> UUIDs;

  using command_line_parser = boost::program_options::command_line_parser;
  using invalid_command_line_syntax = boost::program_options::invalid_command_line_syntax;
  using options_description = boost::program_options::options_description;
  using positional_options_description = boost::program_options::positional_options_description;
  using required_option = boost::program_options::required_option;
  using minutes = std::chrono::minutes;
  using SteadyClock = std::chrono::steady_clock;
  using unknown_option = boost::program_options::unknown_option;
  using variables_map = boost::program_options::variables_map;

  options_description Desc("Allowed options");
  Desc.add_options()
  ("help,?", "print usage message")
    ("friendly-name,n", boost::program_options::value(&FriendlyName)->required(),
   "Device name (regex)")
    ("profile-uuid,u", boost::program_options::value(&UUIDs), "UUID (regex)")
  ("hidp", "Require HID profile to be present")
  ;

  positional_options_description PositionalDesc;
  PositionalDesc.add("friendly-name", 1);
  variables_map VariablesMap;
  try {
    store(command_line_parser(argc, argv)
          .options(Desc)
          .positional(PositionalDesc)
          .run(), VariablesMap);
    notify(VariablesMap);
  } catch (unknown_option &E) {
    std::cerr << E.what() << std::endl << std::endl << Desc << std::endl;
    return EXIT_FAILURE;
  } catch (required_option &E) {
    std::cerr << E.what() << std::endl << std::endl << Desc << std::endl;
    return EXIT_FAILURE;
  } catch (invalid_command_line_syntax &E) {
    std::cerr << E.what () << std::endl << Desc << std::endl;
    return EXIT_FAILURE;
  }

  if (VariablesMap.count("help") > 0) {
    std::cout << Desc << std::endl;
    return EXIT_SUCCESS;
  }

  if (FriendlyName.empty()) {
    std::cerr << "Empty friendly name is not allowed." << std::endl;
    return EXIT_FAILURE;
  }

  for (auto const &UUID: UUIDs) {
    if (UUID.empty()) {
      std::cerr << "Empty UUIDs are not allowed." << std::endl;
      return EXIT_FAILURE;
    }
  }

  if (VariablesMap.count("hidp") > 0) {
    UUIDs.push_back("00000011-0000-1000-8000-00805f9b34fb");
  }

  if (!UUIDs.empty()) {
    std::cout << "Bluetooth Profile UUIDs required to be offered by "
              << "the device:" << std::endl;
    copy(begin(UUIDs), end(UUIDs),
         std::ostream_iterator<std::string>(std::cout, "\n"));
  }

  Bluepairy Bluetooth(FriendlyName, UUIDs);
  auto StartTime = SteadyClock::now();
  auto Timeout = minutes(5);

  Bluetooth.powerUpAllAdapters();

  if (Bluetooth.poweredAdapters().empty()) {
    std::cout << "No Bluetooth adapters available yet." << std::endl;
  }

  while (Bluetooth.usableDevices().empty()) {
    Bluetooth.readWrite();

    {
      auto PairableDevices = Bluetooth.pairableDevices();

      if (!PairableDevices.empty()) {
	for (auto Device: PairableDevices) {
	  std::clog << "Trying to pair with " << Device->name() << std::endl;
	  try {
	    Device->pair();
	    do { Bluetooth.readWrite(); } while (!Device->isPaired());
	    std::clog << "Done pairing!" << std::endl;
	  } catch (BlueZ::Error &E) {
	    std::cerr << "Failed to pair with " << Device->name()
		    << ": " << E.what() << std::endl;
	    Bluetooth.forgetDevice(Device);
	    std::clog << "Forgot device " << Device->name() << std::endl;
	  }
	}
      } else if (!Bluetooth.isDiscovering()) {
	if (Bluetooth.startDiscovery()) {
	  std::cout << "Started discovery mode" << std::endl;
	}
      }
    }

    if (SteadyClock::now() - StartTime > Timeout) {
      std::cout << "Giving up, sorry." << std::endl;

      return EXIT_FAILURE;
    }
  }

  auto UsableDevices = Bluetooth.usableDevices();

  if (UsableDevices.size() == 1) {
    auto Device = UsableDevices.front();
    for (auto UUID: UUIDs) {
      std::clog << "Connecting " << UUID << std::endl;
      Device->connectProfile(UUID);
    }
  }

  if (!UsableDevices.empty()) {
    std::cout << "Found "
	      << (UsableDevices.size() == 1? "one matching device"
		  : "several usable matches")
	      << ":" << std::endl;
    for (auto Device: UsableDevices) {
      std::cout << Device->name() << " (" << Device->address()
		<< ") paired via " << Device->adapter()->address()
		<< std::endl;
    }

    return EXIT_SUCCESS;
  }

  return EXIT_FAILURE;
}

namespace {
  void throwIfErrorIsSet(DBusError &Error) {
    if (dbus_error_is_set(&Error) == TRUE) {
      if (strcmp("org.bluez.Error.AlreadyConnected", Error.name) == 0) {
        BlueZ::AlreadyConnected E(Error.message);
        dbus_error_free(&Error);
        throw E;
      } else if (strcmp("org.bluez.Error.AlreadyExists", Error.name) == 0) {
        BlueZ::AlreadyExists E(Error.message);
        dbus_error_free(&Error);
        throw E;
      } else if (strcmp("org.bluez.Error.AuthenticationFailed",
                        Error.name) == 0) {
        BlueZ::AuthenticationFailed E(Error.message);
        dbus_error_free(&Error);
        throw E;
      } else if (strcmp("org.bluez.Error.AuthenticationRejected",
                        Error.name) == 0) {
        BlueZ::AuthenticationRejected E(Error.message);
        dbus_error_free(&Error);
        throw E;
      } else if (strcmp("org.bluez.Error.AuthenticationTimeout",
                        Error.name) == 0) {
        BlueZ::AuthenticationTimeout E(Error.message);
        dbus_error_free(&Error);
        throw E;
      } else if (strcmp("org.bluez.Error.ConnectionAttemptFailed",
                        Error.name) == 0) {
        BlueZ::ConnectionAttemptFailed E(Error.message);
        dbus_error_free(&Error);
        throw E;
      } else if (strcmp("org.bluez.Error.Failed", Error.name) == 0) {
        BlueZ::Failed E(Error.message);
        dbus_error_free(&Error);
        throw E;
      }

      std::runtime_error E(std::string(Error.name) + ": " + Error.message);
      dbus_error_free(&Error);
      throw E;
    }
  }
} // namespace

namespace BlueZ {
  constexpr char const * const Service = "org.bluez";

  struct Agent {
    static constexpr char const * const Interface = "org.bluez.Agent1";
  };

  struct AgentManager final : public Object {
    static constexpr char const * const Interface = "org.bluez.AgentManager1";

    explicit AgentManager(::Bluepairy *Pairy) : Object("/org/bluez", Pairy) {}

    void registerAgent(char const *AgentPath, char const *Capabilities) const {
      DBusMessage *RegisterAgent = dbus_message_new_method_call
        (Service, path().c_str(), Interface, "RegisterAgent");

      if (RegisterAgent == nullptr) {
        throw std::bad_alloc();
      }

      if (dbus_message_append_args
          (RegisterAgent,
           DBUS_TYPE_OBJECT_PATH, &AgentPath,
           DBUS_TYPE_STRING, &Capabilities,
           DBUS_TYPE_INVALID) == FALSE) {
        throw std::runtime_error
          ("Failed to append arguments to RegisterAgent message");
      }

      DBusError Error;
      dbus_error_init(&Error);
      DBusMessage *Reply = dbus_connection_send_with_reply_and_block
        (Bluepairy->SystemBus, RegisterAgent, -1, &Error);
      dbus_message_unref(RegisterAgent);
      throwIfErrorIsSet(Error);
      if (Reply == nullptr) {
        throw std::bad_alloc();
      }

      dbus_set_error_from_message(&Error, Reply);
      dbus_message_unref(Reply);
      throwIfErrorIsSet(Error);
    }
  };

  constexpr char const * const Adapter::Interface;
  constexpr char const * const Adapter::Property::Address;
  constexpr char const * const Adapter::Property::Discovering;
  constexpr char const * const Adapter::Property::Powered;
  constexpr char const * const Agent::Interface;
  constexpr char const * const AgentManager::Interface;
  constexpr char const * const Device::Interface;
  constexpr char const * const Device::Property::Adapter;
  constexpr char const * const Device::Property::Address;
  constexpr char const * const Device::Property::Connected;
  constexpr char const * const Device::Property::Name;
  constexpr char const * const Device::Property::Paired;
} // namespace BlueZ

namespace DBus {
  namespace interface {
    constexpr char const * const ObjectManager =
      "org.freedesktop.DBus.ObjectManager";
    constexpr char const * const Properties =
      "org.freedesktop.DBus.Properties";
  }
} // namespace DBus

constexpr char const * const Bluepairy::AgentPath;

Bluepairy::Bluepairy
( std::string const &Pattern, std::vector<std::string> UUIDs )
: Pattern(Pattern)
, ExpectedUUIDs(std::move(UUIDs))
, SystemBus([] {
    DBusError Error;
    dbus_error_init(&Error);

    auto Bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &Error);
    throwIfErrorIsSet(Error);

    if (Bus == nullptr) {
      throw std::bad_alloc();
    }

    return Bus;
  }())
{
  sort(begin(ExpectedUUIDs), end(ExpectedUUIDs));

  DBusError Error;

  dbus_error_init(&Error);
  dbus_bus_add_match(SystemBus, "type='signal',sender='org.bluez'", &Error);
  throwIfErrorIsSet(Error);

  readWrite();

  { // Get managed objects
    DBusMessage *GetManagedObjects = dbus_message_new_method_call
      (BlueZ::Service, "/", DBus::interface::ObjectManager, "GetManagedObjects");
    if (!GetManagedObjects) throw std::bad_alloc();

    DBusMessage *ManagedObjects = dbus_connection_send_with_reply_and_block
      (SystemBus, GetManagedObjects, -1, &Error);
    dbus_message_unref(GetManagedObjects);
    throwIfErrorIsSet(Error);
    if (!ManagedObjects) throw std::bad_alloc();
    dbus_set_error_from_message(&Error, ManagedObjects);
    throwIfErrorIsSet(Error);

    DBusMessageIter Args;
    if (!dbus_message_iter_init(ManagedObjects, &Args)) {
      throw std::runtime_error("GetManagedObjects reply was empty");
    }

    if (DBUS_TYPE_ARRAY == dbus_message_iter_get_arg_type(&Args)) {
      DBusMessageIter Objects;

      dbus_message_iter_recurse(&Args, &Objects);
      while (DBUS_TYPE_DICT_ENTRY == dbus_message_iter_get_arg_type(&Objects)) {
        DBusMessageIter Object;
        
        dbus_message_iter_recurse(&Objects, &Object);
        updateObjectProperties(&Object);

        dbus_message_iter_next(&Objects);
      }
      assert(dbus_message_iter_has_next(&Args) == FALSE);
    }
    dbus_message_unref(ManagedObjects);
  }

  auto AgentManager = BlueZ::AgentManager(this);
  AgentManager.registerAgent(AgentPath, "DisplayYesNo");
}

bool BlueZ::Adapter::exists() const {
  for (auto Adapter: Bluepairy->Adapters) {
    if (Adapter.get() == this) return true;
  }

  return false;
}

void BlueZ::Adapter::onPropertiesChanged(DBusMessageIter *Properties /* {sa{sv}}... */)
{
  while (DBUS_TYPE_DICT_ENTRY == dbus_message_iter_get_arg_type(Properties)) {
    DBusMessageIter Property;
    dbus_message_iter_recurse(Properties, &Property);
    if (DBUS_TYPE_STRING ==
        dbus_message_iter_get_arg_type(&Property)) {
      char const *PropertyName;
      dbus_message_iter_get_basic(&Property, &PropertyName);
      dbus_message_iter_next(&Property);
      if (DBUS_TYPE_VARIANT == dbus_message_iter_get_arg_type(&Property)) {
        DBusMessageIter Value;
        dbus_message_iter_recurse(&Property, &Value);
        if (strcmp(Property::Powered, PropertyName) == 0) {
          if (DBUS_TYPE_BOOLEAN == dbus_message_iter_get_arg_type(&Value)) {
            dbus_bool_t BoolValue;
            dbus_message_iter_get_basic(&Value, &BoolValue);
            Powered = BoolValue == TRUE;
          }
        } else if (strcmp(Property::Discovering, PropertyName) == 0) {
          if (DBUS_TYPE_BOOLEAN == dbus_message_iter_get_arg_type(&Value)) {
            dbus_bool_t BoolValue;
            dbus_message_iter_get_basic(&Value, &BoolValue);
	    std::clog << "Set discovering to " << (BoolValue == TRUE? "true" : "false") << std::endl;
            Discovering = BoolValue == TRUE;
          }
        } else if (strcmp(Property::Address, PropertyName) == 0) {
          if (DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&Value)) {
            char const *StringValue;
            dbus_message_iter_get_basic(&Value, &StringValue);
            Address = StringValue;
          }
        } else if (strcmp(Property::Name, PropertyName) == 0) {
          if (DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&Value)) {
            char const *StringValue;
            dbus_message_iter_get_basic(&Value, &StringValue);
            Name = StringValue;
          }
        }
        assert(!dbus_message_iter_has_next(&Property));
      }
    }
    dbus_message_iter_next(Properties);
  }
}

void BlueZ::Adapter::isPowered(bool Value)
{
  DBusMessage *Set = dbus_message_new_method_call
    (Service, path().c_str(), DBus::interface::Properties, "Set");
  if (!Set) throw std::bad_alloc();

  {
    DBusMessageIter Args;

    dbus_message_iter_init_append(Set, &Args);
    dbus_message_iter_append_basic(&Args, DBUS_TYPE_STRING, &Interface);
    dbus_message_iter_append_basic(&Args, DBUS_TYPE_STRING, &Property::Powered);

    DBusMessageIter Variant;
    dbus_bool_t BoolValue = Value? TRUE : FALSE;
    dbus_message_iter_open_container(&Args, DBUS_TYPE_VARIANT, "b", &Variant);
    dbus_message_iter_append_basic(&Variant, DBUS_TYPE_BOOLEAN, &BoolValue);
    dbus_message_iter_close_container(&Args, &Variant);
  }

  DBusError Error;
  dbus_error_init(&Error);
  DBusMessage *Reply = dbus_connection_send_with_reply_and_block
    (Bluepairy->SystemBus, Set, -1, &Error);
  dbus_message_unref(Set);
  throwIfErrorIsSet(Error);
  if (!Reply) throw std::bad_alloc();
  dbus_set_error_from_message(&Error, Reply);
  throwIfErrorIsSet(Error);
  dbus_message_unref(Reply);
}

void BlueZ::Adapter::startDiscovery() const
{
  DBusMessage *StartDiscovery = dbus_message_new_method_call
    (Service, path().c_str(), Interface, "StartDiscovery");
  if (!StartDiscovery) throw std::bad_alloc();

  DBusError Error;
  dbus_error_init(&Error);
  DBusMessage *Reply = dbus_connection_send_with_reply_and_block
    (Bluepairy->SystemBus, StartDiscovery, -1, &Error);
  dbus_message_unref(StartDiscovery);
  throwIfErrorIsSet(Error);
  if (!Reply) throw std::bad_alloc();
  dbus_set_error_from_message(&Error, Reply);
  dbus_message_unref(Reply);
  throwIfErrorIsSet(Error);
}

void BlueZ::Adapter::removeDevice(BlueZ::Device const *Device) const
{
  DBusMessage *RemoveDevice = dbus_message_new_method_call
    (Service, path().c_str(), Interface, "RemoveDevice");
  if (RemoveDevice == nullptr) {
    throw std::bad_alloc();
  }

  char const * const Path = Device->path().c_str();
  if (dbus_message_append_args
      (RemoveDevice, DBUS_TYPE_OBJECT_PATH, &Path, DBUS_TYPE_INVALID) == FALSE) {
    throw std::runtime_error
      ("Failed to append arguments to RemoveDevice message");
  }

  DBusError Error;
  dbus_error_init(&Error);
  DBusMessage *Reply = dbus_connection_send_with_reply_and_block
    (Bluepairy->SystemBus, RemoveDevice, -1, &Error);
  dbus_message_unref(RemoveDevice);
  throwIfErrorIsSet(Error);
  dbus_set_error_from_message(&Error, Reply);
  dbus_message_unref(Reply);
  throwIfErrorIsSet(Error);
}

bool BlueZ::Device::exists() const {
  for (auto Device: Bluepairy->Devices) {
    if (Device.get() == this) return true;
  }

  return false;
}

void BlueZ::Device::onPropertiesChanged(DBusMessageIter *Properties /* {sa{sv}}... */)
{
  while (DBUS_TYPE_DICT_ENTRY == dbus_message_iter_get_arg_type(Properties)) {
    DBusMessageIter Property;
    dbus_message_iter_recurse(Properties, &Property);
    if (DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&Property)) {
      char const *PropertyName;
      dbus_message_iter_get_basic(&Property, &PropertyName);
      dbus_message_iter_next(&Property);
      if (DBUS_TYPE_VARIANT == dbus_message_iter_get_arg_type(&Property)) {
        DBusMessageIter Value;
        dbus_message_iter_recurse(&Property, &Value);
        if (strcmp(Property::Name, PropertyName) == 0) {
          if (DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&Value)) {
            char const *StringValue;
            dbus_message_iter_get_basic(&Value, &StringValue);
            Name = StringValue;
          }
        } else if (strcmp(Property::Address, PropertyName) == 0) {
          if (DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&Value)) {
            char const *StringValue;
            dbus_message_iter_get_basic(&Value, &StringValue);
            Address = StringValue;
          }
        } else if (strcmp(Property::Paired, PropertyName) == 0) {
          if (DBUS_TYPE_BOOLEAN == dbus_message_iter_get_arg_type(&Value)) {
            dbus_bool_t BoolValue;
            dbus_message_iter_get_basic(&Value, &BoolValue);
            Paired = BoolValue == TRUE;
          }
        } else if (strcmp(Property::Connected, PropertyName) == 0) {
          if (DBUS_TYPE_BOOLEAN == dbus_message_iter_get_arg_type(&Value)) {
            dbus_bool_t BoolValue;
            dbus_message_iter_get_basic(&Value, &BoolValue);
            Connected = BoolValue == TRUE;
          }
        } else if (strcmp("UUIDs", PropertyName) == 0) {
          if (DBUS_TYPE_ARRAY == dbus_message_iter_get_arg_type(&Value)) {
            DBusMessageIter UUIDs;

            dbus_message_iter_recurse(&Value, &UUIDs);

            this->UUIDs.clear();
            while (DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&UUIDs)) {
              char const *UUID;
              dbus_message_iter_get_basic(&UUIDs, &UUID);

              this->UUIDs.insert(UUID);
              dbus_message_iter_next(&UUIDs);
            }
          }
        } else if (strcmp(Property::Adapter, PropertyName) == 0) {
          if (DBUS_TYPE_OBJECT_PATH == dbus_message_iter_get_arg_type(&Value)) {
            char const *ObjectPath;
            dbus_message_iter_get_basic(&Value, &ObjectPath);
            if (ObjectPath) {
              AdapterPtr = Bluepairy->getAdapter(ObjectPath);
            } else {
              AdapterPtr.reset();
            }
          }
        }
        assert(!dbus_message_iter_has_next(&Property));
      }
    }
    dbus_message_iter_next(Properties);
  }
}

void BlueZ::Device::pair() const
{
  DBusMessage *Pair = dbus_message_new_method_call
    (Service, path().c_str(), Interface, "Pair");
  if (!Pair) {
    throw std::bad_alloc();
  }

  if (dbus_connection_send(Bluepairy->SystemBus, Pair, nullptr)) {
    dbus_message_unref(Pair);
    while (exists() && !isPaired()) {
      Bluepairy->readWrite();
    }
  } else {
    throw std::runtime_error("Failed to send message");
  }
}

void BlueZ::Device::connectProfile(std::string UUID) const {
  DBusMessage *ConnectProfile = dbus_message_new_method_call
    (Service, path().c_str(), Interface, "ConnectProfile");

  if (ConnectProfile == nullptr) {
    throw std::bad_alloc();
  }

  char const * const Profile = UUID.c_str();
  if (dbus_message_append_args
      (ConnectProfile,
       DBUS_TYPE_STRING, &Profile,
       DBUS_TYPE_INVALID) == FALSE) {
    throw std::runtime_error
      ("Failed to append arguments to ConnectProfile message");
  };

  DBusError Error;
  dbus_error_init(&Error);
  DBusMessage *Reply = dbus_connection_send_with_reply_and_block
    (Bluepairy->SystemBus, ConnectProfile, -1, &Error);
  dbus_message_unref(ConnectProfile);
  throwIfErrorIsSet(Error);
  if (Reply == nullptr) {
    throw std::bad_alloc();
  }

  dbus_set_error_from_message(&Error, Reply);
  dbus_message_unref(Reply);
  throwIfErrorIsSet(Error);
}

std::shared_ptr<BlueZ::Adapter> Bluepairy::getAdapter(char const *Path)
{
  auto EqualPath = [Path](auto O) { return O->path() == Path; };
  auto Pos = find_if(begin(Adapters), end(Adapters), EqualPath);
  if (Pos != end(Adapters)) return *Pos;
      
  Adapters.emplace_back(std::make_shared<BlueZ::Adapter>(Path, this));
  std::clog << "New adapter " << Path << std::endl;
  return Adapters.back();
}

void Bluepairy::removeAdapter(char const *Path)
{
  auto EqualPath = [Path](auto O) { return O->path() == Path; };
  auto Pos = find_if(begin(Adapters), end(Adapters), EqualPath);
  if (Pos != end(Adapters)) {
    Adapters.erase(Pos);
  } else {
    std::clog << "WARNING: Tried to remove adapter we never knew about." << std::endl;
  }
}

std::shared_ptr<BlueZ::Device> Bluepairy::getDevice(char const *Path)
{
  auto EqualPath = [Path](auto O) { return O->path() == Path; };
  auto Pos = find_if(begin(Devices), end(Devices), EqualPath);
  if (Pos != end(Devices)) return *Pos;
      
  Devices.push_back(std::make_shared<BlueZ::Device>(Path, this));
  std::clog << "New device " << Path << std::endl;
  return Devices.back();
}

void Bluepairy::removeDevice(char const *Path)
{
  auto EqualPath = [Path](auto O) { return O->path() == Path; };
  auto Pos = find_if(begin(Devices), end(Devices), EqualPath);
  if (Pos != end(Devices)) {
    Devices.erase(Pos);
  } else {
    std::clog << "WARNING: Tried to remove device we never knew about." << std::endl;
  }
}

void Bluepairy::updateObjectProperties(DBusMessageIter *Object /* oa{sa{sv}} */)
{
  if (DBUS_TYPE_OBJECT_PATH == dbus_message_iter_get_arg_type(Object)) {
    char const *Path;

    dbus_message_iter_get_basic(Object, &Path);
    dbus_message_iter_next(Object);
    if (DBUS_TYPE_ARRAY == dbus_message_iter_get_arg_type(Object)) {
      DBusMessageIter Interfaces;

      dbus_message_iter_recurse(Object, &Interfaces);
      while (DBUS_TYPE_DICT_ENTRY ==
             dbus_message_iter_get_arg_type(&Interfaces)) {
        DBusMessageIter Interface;

        dbus_message_iter_recurse(&Interfaces, &Interface);
        if (DBUS_TYPE_STRING ==
            dbus_message_iter_get_arg_type(&Interface)) {
          char const *InterfaceName;

          dbus_message_iter_get_basic(&Interface, &InterfaceName);
          if (strcmp(BlueZ::Adapter::Interface, InterfaceName) == 0) {
            dbus_message_iter_next(&Interface);
            if (DBUS_TYPE_ARRAY ==
                dbus_message_iter_get_arg_type(&Interface)) {
              DBusMessageIter Properties;

              dbus_message_iter_recurse(&Interface, &Properties);
              getAdapter(Path)->onPropertiesChanged(&Properties);

              assert(dbus_message_iter_has_next(&Interface) == FALSE);
            }
          } else if (strcmp(BlueZ::Device::Interface, InterfaceName) == 0) {
            dbus_message_iter_next(&Interface);
            if (DBUS_TYPE_ARRAY ==
                dbus_message_iter_get_arg_type(&Interface)) {
              DBusMessageIter Properties;
              dbus_message_iter_recurse(&Interface, &Properties);
              getDevice(Path)->onPropertiesChanged(&Properties);
              assert(dbus_message_iter_has_next(&Interface) == FALSE);
            }
          }
        }
        dbus_message_iter_next(&Interfaces);
      }
      assert(dbus_message_iter_has_next(Object) == FALSE);
    }
  }
}

void Bluepairy::readWrite()
{
  DBusMessage *Incoming;

  dbus_connection_read_write(SystemBus, 100);

  while ((Incoming = dbus_connection_pop_message(SystemBus))) {
    char const *Path = dbus_message_get_path(Incoming);

    switch (dbus_message_get_type(Incoming)) {
    case DBUS_MESSAGE_TYPE_ERROR: {
      DBusError Error;
      dbus_error_init(&Error);
      dbus_uint32_t reply_serial = dbus_message_get_reply_serial(Incoming);
      // According to tests, reply_serial is wrong, bluez bug?
      if (dbus_set_error_from_message(&Error, Incoming)) {
        throwIfErrorIsSet(Error);
      }
      break;
    }

    case DBUS_MESSAGE_TYPE_METHOD_RETURN: {
      auto ReplySerial = dbus_message_get_reply_serial(Incoming);
      std::cout << "Method return " << ReplySerial << std::endl;
      break;
    }

    case DBUS_MESSAGE_TYPE_METHOD_CALL:
      if (dbus_message_has_path(Incoming, AgentPath)) {
        if (dbus_message_is_method_call
            (Incoming, BlueZ::Agent::Interface, "RequestPinCode") == TRUE) {
          DBusMessageIter Args;

          dbus_message_iter_init(Incoming, &Args);
          if (DBUS_TYPE_OBJECT_PATH == dbus_message_iter_get_arg_type(&Args)) {
            char const *Path;

            dbus_message_iter_get_basic(&Args, &Path);
            auto Device = getDevice(Path);
            DBusMessage *Reply = dbus_message_new_method_return(Incoming);
            if (Reply != nullptr) {
              char const * const PIN = guessPIN(Device).c_str();
              dbus_message_append_args(Reply, DBUS_TYPE_STRING, &PIN, DBUS_TYPE_INVALID);
              dbus_connection_send(SystemBus, Reply, nullptr);
              dbus_message_unref(Reply);
              dbus_connection_flush(SystemBus);
              std::clog << "RequestPinCode for " << Device->name()
                        << " answered with " << PIN << std::endl;
            }
          }
        } else if (dbus_message_is_method_call
                   (Incoming, BlueZ::Agent::Interface, "RequestConfirmation")
                   == TRUE) {
          char const *Path;
          dbus_uint32_t PassKey;
          DBusError Error;
          dbus_error_init(&Error);
          if (dbus_message_get_args
              (Incoming, &Error,
               DBUS_TYPE_OBJECT_PATH, &Path,
               DBUS_TYPE_UINT32, &PassKey,
               DBUS_TYPE_INVALID) == FALSE) {
            throw std::runtime_error
              ("Failed to get arguments of RequestConfirmation message");
          }
          throwIfErrorIsSet(Error);
          // A void reply indicates that we confirm.
          DBusMessage *Reply = dbus_message_new_method_return(Incoming);
          if (Reply == nullptr) {
            throw std::bad_alloc();
          }
          dbus_connection_send(SystemBus, Reply, nullptr);
          dbus_message_unref(Reply);
          dbus_connection_flush(SystemBus);
          std::clog << "RequestConfirmation confirmed" << std::endl;
        }
      }
      std::clog << "Method call "
                << dbus_message_get_path(Incoming) << " "
                << dbus_message_get_interface(Incoming) << " "
                << dbus_message_get_member(Incoming)
                << std::endl;
      break;
    case DBUS_MESSAGE_TYPE_SIGNAL:
      if (dbus_message_is_signal
          (Incoming, DBus::interface::Properties, "PropertiesChanged")
          == TRUE) {
        DBusMessageIter Args;

        dbus_message_iter_init(Incoming, &Args);
        if (DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&Args)) {
          char const *InterfaceName;

          dbus_message_iter_get_basic(&Args, &InterfaceName);
          dbus_message_iter_next(&Args);
          if (DBUS_TYPE_ARRAY == dbus_message_iter_get_arg_type(&Args)) {
            DBusMessageIter Properties;

            dbus_message_iter_recurse(&Args, &Properties);

            if (strcmp(BlueZ::Adapter::Interface, InterfaceName) == 0) {
              getAdapter(Path)->onPropertiesChanged(&Properties);
            } else if (strcmp(BlueZ::Device::Interface, InterfaceName) == 0) {
              getDevice(Path)->onPropertiesChanged(&Properties);
            }
          }
        }
      } else if (dbus_message_has_interface(Incoming, DBus::interface::ObjectManager) == TRUE) {
        if (dbus_message_has_member(Incoming, "InterfacesAdded") == TRUE) {
          DBusMessageIter Args;
          dbus_message_iter_init(Incoming, &Args);

          updateObjectProperties(&Args);
        } else if (dbus_message_has_member(Incoming, "InterfacesRemoved") == TRUE) {
          DBusMessageIter Args;
          dbus_message_iter_init(Incoming, &Args);

          if (DBUS_TYPE_OBJECT_PATH == dbus_message_iter_get_arg_type(&Args)) {
            char const *Path;

            dbus_message_iter_get_basic(&Args, &Path);
            dbus_message_iter_next(&Args);
            if (DBUS_TYPE_ARRAY == dbus_message_iter_get_arg_type(&Args)) {
              DBusMessageIter Interfaces;

              dbus_message_iter_recurse(&Args, &Interfaces);
              while (DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&Interfaces)) {
                char const *InterfaceName;

                dbus_message_iter_get_basic(&Interfaces, &InterfaceName);
                if (strcmp(BlueZ::Adapter::Interface, InterfaceName) == 0) {
		  removeAdapter(Path);
                } else if (strcmp(BlueZ::Device::Interface, InterfaceName) == 0) {
		  removeDevice(Path);
                }
                dbus_message_iter_next(&Interfaces);
              }
            }
          }
        }
      }
      fprintf(stderr, "Signal %s.%s\n",
              dbus_message_get_interface(Incoming),
              dbus_message_get_member(Incoming));
      break;
    }
    dbus_message_unref(Incoming);
  }
}

std::string Bluepairy::guessPIN(std::shared_ptr<BlueZ::Device> Device) const
{
  std::smatch Match;
  std::regex HandyTech("(" "Actilino ALO"
                       "|" "Active Braille AB4"
                       "|" "Active Star AS4"
                       "|" "Basic Braille BB4"
                       "|" "Braille Star BS4"
                       "|" "Braillino BL2"
                       ")"
                       "/" "[[:upper:]][[:digit:]]"
                       "-" "([[:digit:]]+)");

  if (regex_match(Device->name(), Match, HandyTech) && Match.size() == 3) {
    std::string SerialNumber = Match[2];

    if (SerialNumber.size() == 5) {
      std::stringstream PINCode;

      for (int I = 0; I < SerialNumber.size(); ++I) {
        char Digit = ((SerialNumber[I] - '0' + I + 1) % 10) + '0';
        PINCode << Digit;
      }

      return PINCode.str();
    }
  }

  return "0000";
}

void Bluepairy::powerUpAllAdapters() {
  for (auto Adapter: Adapters) {
    if (!Adapter->isPowered()) {
      std::clog << "Powering up adapter " << Adapter->name() << std::endl;
      Adapter->isPowered(true);
      auto StartTime = std::chrono::steady_clock::now();
      do {
	readWrite();
      } while (Adapter->exists() && !Adapter->isPowered() &&
	       std::chrono::steady_clock::now() - StartTime < std::chrono::seconds(1));
      if (Adapter->isPowered()) {
	std::clog << "Adapter " << Adapter->name()
		  << " is now powered up." << std::endl;
      } else {
	std::cerr << "Failed to power up adapter "
		  << Adapter->name() << ", ignored."
		  << std::endl;
      }
    }
  }
}

bool Bluepairy::isDiscovering() const {
  for (auto Adapter: Adapters) {
    if (Adapter->isPowered() && Adapter->isDiscovering()) {
      return true;
    }
  }

  return false;
}

bool Bluepairy::startDiscovery() {
  bool Started = false;

  for (auto Adapter: poweredAdapters()) {
    if (!Adapter->isDiscovering()) {
      Adapter->startDiscovery();
      do { readWrite(); } while (Adapter->exists() && !Adapter->isDiscovering());
      if (Adapter->exists() && Adapter->isDiscovering()) {
	Started = true;
      }
    }
  }

  return Started;
}

void Bluepairy::forgetDevice(std::shared_ptr<BlueZ::Device> Device) {
  Device->adapter()->removeDevice(Device.get());
  while (Device->exists()) {
    readWrite();
  }
}
