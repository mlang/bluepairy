#if !defined(BLUEPAIRY_HPP)
#define BLUEPAIRY_HPP

#include <memory>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <dbus/dbus.h>

class Bluepairy;

namespace BlueZ {
  struct Error: std::runtime_error {
    Error(char const *Message) : std::runtime_error(Message) {}
    Error(Error const &E) noexcept = default;
  };
  struct AlreadyConnected : Error {
    AlreadyConnected(char const *Message) : Error(Message) {}
  };
  struct AlreadyExists : Error {
    AlreadyExists(char const *Message) : Error(Message) {}
  };
  struct AuthenticationFailed: Error {
    AuthenticationFailed(char const *Message) : Error(Message) {}
  };
  struct AuthenticationRejected : Error {
    AuthenticationRejected(char const *Message) : Error(Message) {}
  };
  struct AuthenticationTimeout : Error {
    AuthenticationTimeout(char const *Message) : Error(Message) {}
  };
  struct ConnectionAttemptFailed: Error {
    ConnectionAttemptFailed(char const *Message) : Error(Message) {}
  };

  class Object {
    std::string const Path;

  protected:
    ::Bluepairy * const Bluepairy;
    Object(std::string const &Path, ::Bluepairy *Pairy)
    : Path{Path}, Bluepairy{Pairy} {
    }

  public:
    std::string const &path() const { return Path; }
  };

  class Device;

  class Adapter final : public Object {
    std::string Address;
    std::string Name;
    bool Powered;
    bool Discovering;

  public:
    static constexpr char const * const Interface = "org.bluez.Adapter1";
    struct Property {
      static constexpr char const * const Address = "Address";
      static constexpr char const * const Name = "Name";
      static constexpr char const * const Discovering = "Discovering";
      static constexpr char const * const Powered = "Powered";
    };
    Adapter(std::string const &Path, ::Bluepairy *Pairy)
    : Object(Path, Pairy) {}

    bool exists() const;
    void onPropertiesChanged(DBusMessageIter *);

    std::string const &address() const { return Address; }
    std::string const &name() const { return Name; }
    bool isPowered() const { return Powered; }
    void isPowered(bool);
    bool isDiscovering() const { return Discovering; }

    void startDiscovery() const;
    void removeDevice(Device const *) const;
  };

  class AgentManager;

  class Device final : public Object {
    std::shared_ptr<Adapter> AdapterPtr;
    std::string Address;
    bool Connected;
    std::string Name;
    bool Paired;
    std::set<std::string> UUIDs;

  public:
    static constexpr char const * const Interface = "org.bluez.Device1";
    struct Property {
      static constexpr char const * const Adapter = "Adapter";
      static constexpr char const * const Address = "Address";
      static constexpr char const * const Connected = "Connected";
      static constexpr char const * const Name = "Name";
      static constexpr char const * const Paired = "Paired";
    };

    Device(std::string const &Path, ::Bluepairy *Pairy) : Object(Path, Pairy) {}
    bool exists() const;

    void onPropertiesChanged(DBusMessageIter *);

    std::string const &address() const { return Address; }
    std::shared_ptr<Adapter const> adapter() const { return AdapterPtr; }
    std::string const &name() const { return Name; }
    bool isPaired() const { return Paired; }
    bool isConnected() const { return Connected; }
    std::set<std::string> const &profiles() const { return UUIDs; }

    void pair() const;

    void connectProfile(std::string) const;
  };
}

class Bluepairy final {
  static constexpr char const * const AgentPath = "/bluepairy/agent";

  std::regex Pattern;
  std::vector<std::string> ExpectedUUIDs;
  DBusConnection *SystemBus;

  std::vector<std::shared_ptr<BlueZ::Adapter>> Adapters;
  decltype(Adapters)::value_type getAdapter(char const *Path);
  void removeAdapter(char const *Path);
    
  std::vector<std::shared_ptr<BlueZ::Device>> Devices;
  decltype(Devices)::value_type getDevice(char const *Path);
  void removeDevice(char const *Path);

  void updateObjectProperties(DBusMessageIter *);

  friend class BlueZ::Adapter;
  friend class BlueZ::AgentManager;
  friend class BlueZ::Device;

public:
  Bluepairy(std::string const &Pattern, std::vector<std::string> UUIDs);
  Bluepairy(Bluepairy const &) = delete;
  Bluepairy(Bluepairy &&) = delete;
  Bluepairy &operator= (Bluepairy &&) = delete;
  Bluepairy &operator= (Bluepairy const &) = delete;
  ~Bluepairy() {
    dbus_connection_close(SystemBus);
    dbus_connection_unref(SystemBus);
  }

  void readWrite();
    
  bool nameMatches(std::shared_ptr<BlueZ::Device> Device) const {
    return regex_search(Device->name(), Pattern, std::regex_constants::match_not_null);
  }
  bool hasExpectedProfiles(std::shared_ptr<BlueZ::Device> Device) const {
    std::vector<std::string> intersection;
    std::set_intersection(Device->profiles().begin(), Device->profiles().end(),
                          ExpectedUUIDs.begin(), ExpectedUUIDs.end(),
                          std::back_inserter(intersection));
    return intersection == ExpectedUUIDs;
  }
  decltype(Devices) usableDevices() const {
    decltype(Devices) Result;
    for (auto Device: Devices) {
      if (Device->adapter()->isPowered() && Device->isPaired() &&
	  this->nameMatches(Device) && this->hasExpectedProfiles(Device)) {
	Result.push_back(Device);
      }
    }

    return Result;
  }

  decltype(Devices) pairableDevices() const {
    decltype(Devices) Result;
    for (auto Device: Devices) {
      if (Device->adapter()->exists() && Device->adapter()->isPowered() &&
	  !Device->isPaired() && this->nameMatches(Device) &&
	  this->hasExpectedProfiles(Device)) {
	Result.push_back(Device);
      }
    }

    return Result;
  }

  decltype(Adapters) poweredAdapters() const {
    decltype(Adapters) Result;

    for (auto Adapter: Adapters) {
      if (Adapter->isPowered()) {
	Result.push_back(Adapter);
      }
    }

    return Result;
  }

  std::string guessPIN(std::shared_ptr<BlueZ::Device>) const;

  void powerUpAllAdapters();
  bool isDiscovering() const;
  bool startDiscovery();

  void forgetDevice(decltype(Devices)::value_type);
};

#endif // BLUEPAIRY_HPP
